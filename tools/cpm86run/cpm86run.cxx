// cpm86run: run a CP/M-86 .CMD program on Linux, macOS or Windows.
//
// The 8086 is David Lee's emulator from ntvdm (i8086.cxx, CC0). This file
// loads a CMD file as the CP/M-86 CCP does and emulates the BDOS (INT E0h):
// the console, and files in the host's current directory through FCBs. It
// is meant for running test programs from scripts, not for running CP/M-86
// itself: there is no BIOS, disk or CCP.
//
//   cpm86run [-t] [-e] [-m] program[.cmd] [arguments]
//
//   -t  trace instructions and BDOS calls to cpm86run.log
//   -e  exit with the program's return code (BDOS function 108, as in
//       CP/M-86 Plus); otherwise 0
//   -m  print the memory model and group addresses on stderr
//
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <djl_os.hxx>

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <djltrace.hxx>
#include "i8086.hxx"

using namespace std;

CDJLTrace tracer;

// Low memory: the interrupt vectors, then the BDOS and exit stubs at
// 0040:0000, then the CCP stack. Programs load from paragraph 0100h up to
// the end of conventional memory.
static const uint16_t StubSeg = 0x0040;
static const uint16_t BdosStub = 0x0000;   // int 69h; db 0E0h; iret
static const uint16_t ExitStub = 0x0010;   // int 69h; db 0FFh; hlt
static const uint16_t CcpSeg = 0x0050;     // 96-byte CCP stack below 0100h
static const uint16_t CcpSp = 0x00f0;
static const uint16_t TpaStart = 0x0100;   // paragraphs
static const uint16_t TpaEnd = 0xa000;

static bool g_trace = false;
static bool g_done = false;
static uint16_t g_returnCode = 0;
static uint16_t g_dmaSeg = 0, g_dmaOff = 0x80;
static uint16_t g_freeSeg = TpaStart;      // first paragraph not allocated
static uint8_t g_user = 0;
static uint8_t g_disk = 0;

// Directory search state (functions 17 and 18).
static vector<string> g_search;
static size_t g_searchNext = 0;

void i8086_hard_exit( const char * pcerror )
{
    fprintf( stderr, "cpm86run: %s", pcerror );
    tracer.Trace( "hard exit: %s", pcerror );
    cpu.trace_state();
    exit( 125 );
}

void i8086_invoke_halt()
{
    g_done = true;
    cpu.end_emulation();
}

uint8_t i8086_invoke_in_byte( uint16_t ) { return 0xff; }
uint16_t i8086_invoke_in_word( uint16_t ) { return 0xffff; }
void i8086_invoke_out_byte( uint16_t, uint8_t ) {}
void i8086_invoke_out_word( uint16_t, uint16_t ) {}

static uint8_t * mem( uint16_t seg, uint16_t off ) { return memory + ( ( ( (uint32_t) seg << 4 ) + off ) & 0xfffff ); }
static uint16_t rd16( uint16_t seg, uint16_t off ) { uint8_t * p = mem( seg, off ); return p[0] | ( p[1] << 8 ); }
static void wr16( uint16_t seg, uint16_t off, uint16_t v ) { uint8_t * p = mem( seg, off ); p[0] = v & 0xff; p[1] = v >> 8; }

// ---------------------------------------------------------------------------
// Console

#ifndef _WIN32
static struct termios g_oldTermios;
static bool g_rawTerminal = false;

static void restore_terminal()
{
    if ( g_rawTerminal )
        tcsetattr( 0, TCSANOW, &g_oldTermios );
}

static void raw_terminal()
{
    if ( g_rawTerminal || !isatty( 0 ) )
        return;
    if ( tcgetattr( 0, &g_oldTermios ) != 0 )
        return;
    struct termios t = g_oldTermios;
    t.c_lflag &= ~( ICANON | ECHO );
    t.c_cc[ VMIN ] = 1;
    t.c_cc[ VTIME ] = 0;
    tcsetattr( 0, TCSANOW, &t );
    g_rawTerminal = true;
    atexit( restore_terminal );
}

static bool key_available()
{
    fd_set fds;
    FD_ZERO( &fds );
    FD_SET( 0, &fds );
    struct timeval tv = { 0, 0 };
    return select( 1, &fds, 0, 0, &tv ) > 0;
}
#else
static void raw_terminal() {}
static bool key_available() { return true; }
#endif

static int g_pendingKey = -1;
static bool g_stdinEof = false;

// A key, or 1Ah (^Z) at the end of input. Line ends become CR, as from a
// terminal.
static uint8_t get_key()
{
    if ( g_pendingKey >= 0 )
    {
        uint8_t c = (uint8_t) g_pendingKey;
        g_pendingKey = -1;
        return c;
    }
    fflush( stdout );
    raw_terminal();
    int c = g_stdinEof ? EOF : getchar();
    if ( c == EOF )
    {
        g_stdinEof = true;
        return 0x1a;
    }
    if ( c == '\n' )
        return '\r';
    return (uint8_t) c;
}

static bool console_status()
{
    if ( g_pendingKey >= 0 )
        return true;
    if ( g_stdinEof )
        return false;
    fflush( stdout );
    return key_available();
}

static void put_char( uint8_t c )
{
    // CP/M programs end lines with CR LF; the host terminal wants LF.
    if ( c == '\r' )
        return;
    putchar( c );
}

// Function 10: read a line into the buffer at DS:DX (max, count, text).
// The line is echoed as CP/M echoes typing, control characters as ^X, so
// the output reads as it would on a terminal even with input from a file.
// The BDOS leaves the cursor at the end of the line.
static void echo( uint8_t c )
{
    if ( c < 0x20 )
    {
        put_char( '^' );
        put_char( c + '@' );
    }
    else
        put_char( c );
}

static void read_console_buffer( uint16_t seg, uint16_t off )
{
    uint8_t max = *mem( seg, off );
    uint8_t n = 0;
    bool tty = isatty( 0 );
    for ( ;; )
    {
        uint8_t c = get_key();
        if ( c == '\r' || c == '\n' )
            break;
        if ( c == 0x1a && g_stdinEof )
        {
            // The end of the host's input: a line holding just ^Z, as a
            // user would type at the end of input.
            if ( n == 0 && max > 0 )
            {
                *mem( seg, off + 2 + n++ ) = c;
                echo( c );
            }
            break;
        }
        if ( ( c == 8 || c == 0x7f ) && n > 0 )
        {
            n--;
            if ( tty ) fputs( "\b \b", stdout );
            continue;
        }
        if ( n < max )
        {
            *mem( seg, off + 2 + n ) = c;
            n++;
            echo( c );
        }
    }
    *mem( seg, off + 1 ) = n;
}

// ---------------------------------------------------------------------------
// Files: an FCB names a file in the host's current directory. Names are
// matched without regard to case, and new files get lower-case names.

struct Fcb
{
    uint8_t dr;
    char f[ 8 ];
    char t[ 3 ];
    uint8_t ex, s1, s2, rc;
    uint8_t d[ 16 ];
    uint8_t cr;
    uint8_t r0, r1, r2;
};

static Fcb * fcb_at( uint16_t seg, uint16_t off ) { return (Fcb *) mem( seg, off ); }

static string fcb_name( const Fcb * f )
{
    string name;
    for ( int i = 0; i < 8 && ( f->f[ i ] & 0x7f ) != ' '; i++ )
        name += (char) tolower( f->f[ i ] & 0x7f );
    string ext;
    for ( int i = 0; i < 3 && ( f->t[ i ] & 0x7f ) != ' '; i++ )
        ext += (char) tolower( f->t[ i ] & 0x7f );
    if ( !ext.empty() )
        name += "." + ext;
    return name;
}

// The 11 bytes of a host name in FCB form, or false when it has no 8.3 form.
static bool to_fcb_name( const char * host, char out[ 11 ] )
{
    memset( out, ' ', 11 );
    const char * dot = strrchr( host, '.' );
    size_t base = dot ? (size_t) ( dot - host ) : strlen( host );
    size_t ext = dot ? strlen( dot + 1 ) : 0;
    if ( base == 0 || base > 8 || ext > 3 )
        return false;
    for ( size_t i = 0; i < base; i++ )
    {
        if ( host[ i ] == '.' || host[ i ] <= ' ' || host[ i ] == '?' || host[ i ] == '*' )
            return false;
        out[ i ] = (char) toupper( host[ i ] );
    }
    for ( size_t i = 0; i < ext; i++ )
        out[ 8 + i ] = (char) toupper( dot[ 1 + i ] );
    return true;
}

static vector<string> list_directory()
{
    vector<string> names;
    DIR * d = opendir( "." );
    if ( !d )
        return names;
    while ( struct dirent * e = readdir( d ) )
    {
        struct stat st;
        char n[ 11 ];
        if ( stat( e->d_name, &st ) == 0 && S_ISREG( st.st_mode ) && to_fcb_name( e->d_name, n ) )
            names.push_back( e->d_name );
    }
    closedir( d );
    return names;
}

static bool fcb_matches( const Fcb * f, const char * host )
{
    char n[ 11 ];
    if ( !to_fcb_name( host, n ) )
        return false;
    const char * pat = (const char *) f + 1; // the name and type
    for ( int i = 0; i < 11; i++ )
    {
        char p = (char) toupper( pat[ i ] & 0x7f );
        if ( p != '?' && p != n[ i ] )
            return false;
    }
    return true;
}

// The host name of the file an FCB names, or "" when there is none.
static string find_file( const Fcb * f )
{
    for ( const string & n : list_directory() )
        if ( fcb_matches( f, n.c_str() ) )
            return n;
    return "";
}

static bool has_wildcards( const Fcb * f )
{
    const uint8_t * name = (const uint8_t *) f + 1; // the name and type
    for ( int i = 0; i < 11; i++ )
        if ( ( name[ i ] & 0x7f ) == '?' )
            return true;
    return false;
}

static long file_size( const string & name )
{
    struct stat st;
    return stat( name.c_str(), &st ) == 0 ? (long) st.st_size : -1;
}

// The record an FCB's sequential position names: s2, ex and cr.
static uint32_t seq_record( const Fcb * f )
{
    return ( (uint32_t) ( f->s2 & 0x3f ) << 12 ) | ( (uint32_t) ( f->ex & 0x1f ) << 7 ) | ( f->cr & 0x7f );
}

static void set_seq_record( Fcb * f, uint32_t rec )
{
    f->cr = rec & 0x7f;
    f->ex = ( rec >> 7 ) & 0x1f;
    f->s2 = ( rec >> 12 ) & 0x3f;
}

static uint32_t rand_record( const Fcb * f ) { return f->r0 | ( f->r1 << 8 ) | ( ( f->r2 & 3 ) << 16 ); }

static void set_rand_record( Fcb * f, uint32_t rec )
{
    f->r0 = rec & 0xff;
    f->r1 = ( rec >> 8 ) & 0xff;
    f->r2 = ( rec >> 16 ) & 0xff;
}

// rc: the records of the current extent that the file has.
static void set_rc( Fcb * f, const string & name )
{
    long size = file_size( name );
    uint32_t records = size < 0 ? 0 : (uint32_t) ( ( size + 127 ) / 128 );
    uint32_t extentStart = ( seq_record( f ) & ~0x7fu );
    uint32_t left = records > extentStart ? records - extentStart : 0;
    f->rc = (uint8_t) ( left > 128 ? 128 : left );
}

// The FCB's file: stored in d[] by open and make (as an index into a table
// of names), so that reads and writes need not search the directory.
static vector<string> g_openNames;

static string fcb_file( Fcb * f )
{
    uint16_t idx = f->d[ 0 ] | ( f->d[ 1 ] << 8 );
    if ( f->d[ 2 ] == 0xc8 && f->d[ 3 ] == 0x86 && idx < g_openNames.size() )
    {
        // The name must still match: the FCB may have been reused.
        if ( fcb_matches( f, g_openNames[ idx ].c_str() ) )
            return g_openNames[ idx ];
    }
    return find_file( f );
}

static void remember_file( Fcb * f, const string & name )
{
    uint16_t idx = 0;
    for ( ; idx < g_openNames.size(); idx++ )
        if ( g_openNames[ idx ] == name )
            break;
    if ( idx == g_openNames.size() )
        g_openNames.push_back( name );
    memset( f->d, 0, sizeof( f->d ) );
    f->d[ 0 ] = idx & 0xff;
    f->d[ 1 ] = idx >> 8;
    f->d[ 2 ] = 0xc8;
    f->d[ 3 ] = 0x86;
}

// Read record rec of the file into the DMA buffer: 0, or 1 at the end of
// the file. A short last record is filled with ^Z.
static uint8_t read_record( const string & name, uint32_t rec )
{
    FILE * fp = fopen( name.c_str(), "rb" );
    if ( !fp )
        return 1;
    uint8_t buf[ 128 ];
    size_t n = 0;
    if ( fseek( fp, (long) rec * 128, SEEK_SET ) == 0 )
        n = fread( buf, 1, 128, fp );
    fclose( fp );
    if ( n == 0 )
        return 1;
    memset( buf + n, 0x1a, 128 - n );
    memcpy( mem( g_dmaSeg, g_dmaOff ), buf, 128 );
    return 0;
}

static uint8_t write_record( const string & name, uint32_t rec, bool zeroFill )
{
    FILE * fp = fopen( name.c_str(), "r+b" );
    if ( !fp )
        return 2;
    long end = file_size( name );
    long pos = (long) rec * 128;
    if ( pos > end )
    {
        // A random write past the end leaves a gap, which CP/M reads as
        // zeros with function 40 (and as whatever was on the disk otherwise).
        fseek( fp, end, SEEK_SET );
        static const uint8_t zero[ 128 ] = { 0 };
        for ( long p = end; p < pos; p += 128 )
            fwrite( zero, 1, ( pos - p ) < 128 ? (size_t) ( pos - p ) : 128, fp );
    }
    (void) zeroFill;
    fseek( fp, pos, SEEK_SET );
    size_t n = fwrite( mem( g_dmaSeg, g_dmaOff ), 1, 128, fp );
    fclose( fp );
    return n == 128 ? 0 : 2;
}

static void dir_entry( const string & name )
{
    uint8_t * p = mem( g_dmaSeg, g_dmaOff );
    memset( p, 0xe5, 128 );
    memset( p, 0, 32 );
    p[ 0 ] = g_user;
    char n[ 11 ];
    to_fcb_name( name.c_str(), n );
    memcpy( p + 1, n, 11 );
    long size = file_size( name );
    uint32_t records = size < 0 ? 0 : (uint32_t) ( ( size + 127 ) / 128 );
    // The last extent, as a directory with 16 KiB extents would have.
    uint32_t extents = records ? ( records - 1 ) / 128 : 0;
    p[ 12 ] = extents & 0x1f;
    p[ 14 ] = ( extents >> 5 ) & 0x3f;
    p[ 15 ] = (uint8_t) ( records - extents * 128 );
    for ( int i = 0; i < 16 && (uint32_t) i * 8 < records - extents * 128 + 7; i++ )
        p[ 16 + i ] = (uint8_t) ( i + 1 );
}

static uint8_t search_next()
{
    if ( g_searchNext >= g_search.size() )
        return 0xff;
    dir_entry( g_search[ g_searchNext++ ] );
    return 0;
}

// ---------------------------------------------------------------------------
// Memory: one free region from g_freeSeg to TpaEnd.

// The memory control block at DS:DX: M-Base, M-Length, M-Ext.
static uint8_t get_max_mem( uint16_t seg, uint16_t off, bool allocate, bool any )
{
    uint16_t want = rd16( seg, off + 2 );
    uint16_t avail = TpaEnd - g_freeSeg;
    if ( avail == 0 || ( !any && want > avail ) )
        return 0xff;
    uint16_t len = any ? avail : want;
    wr16( seg, off, g_freeSeg );
    wr16( seg, off + 2, len );
    *mem( seg, off + 4 ) = 0;
    if ( allocate )
        g_freeSeg += len;
    return 0;
}

// ---------------------------------------------------------------------------
// BDOS

static void bdos()
{
    uint8_t fn = cpu.cl();
    uint16_t dx = cpu.get_dx();
    uint16_t ds = cpu.get_ds();
    uint16_t result = 0;
    bool setResult = true;

    if ( g_trace )
    {
        tracer.Trace( "bdos %u dx %04x ds %04x\n", fn, dx, ds );
        if ( ( fn >= 15 && fn <= 23 ) || ( fn >= 30 && fn <= 36 ) || fn == 40 )
        {
            const uint8_t * p = mem( ds, dx );
            tracer.Trace( "  fcb %02x '%.11s' ex %02x s2 %02x rc %02x cr %02x r %02x%02x%02x\n",
                          p[0], (const char *) p + 1, p[12], p[14], p[15], p[32], p[35], p[34], p[33] );
        }
    }

    switch ( fn )
    {
        case 0: // system reset: end the program
            g_done = true;
            cpu.end_emulation();
            return;
        case 1: // console input, echoed
        {
            uint8_t c = get_key();
            put_char( c );
            result = c;
            break;
        }
        case 2: put_char( cpu.dl() ); break;
        case 3: result = get_key(); break;          // reader: the console
        case 4: put_char( cpu.dl() ); break;        // punch
        case 5: break;                              // list: discarded
        case 6:
            if ( cpu.dl() == 0xff )
                result = console_status() ? get_key() : 0;
            else if ( cpu.dl() == 0xfe )
                result = console_status() ? 0xff : 0;
            else
                put_char( cpu.dl() );
            break;
        case 7: result = 0; break;                  // get IOBYTE
        case 8: break;                              // set IOBYTE
        case 9:
        {
            for ( uint16_t o = dx; *mem( ds, o ) != '$'; o++ )
                put_char( *mem( ds, o ) );
            break;
        }
        case 10: read_console_buffer( ds, dx ); break;
        case 11: result = console_status() ? 0xff : 0; break;
        case 12: result = 0x0022; break;            // CP/M-86 1.1: version 2.2
        case 13: g_dmaSeg = ds; g_dmaOff = 0x80; g_disk = 0; break;
        case 14: g_disk = cpu.dl(); break;
        case 15: // open
        {
            Fcb * f = fcb_at( ds, dx );
            string name = find_file( f );
            if ( name.empty() ) { result = 0xff; break; }
            remember_file( f, name );
            f->s2 = 0;
            set_rc( f, name );
            result = 0;
            break;
        }
        case 16: // close
        {
            Fcb * f = fcb_at( ds, dx );
            result = fcb_file( f ).empty() ? 0xff : 0;
            break;
        }
        case 17: // search first
        {
            Fcb * f = fcb_at( ds, dx );
            g_search.clear();
            g_searchNext = 0;
            for ( const string & n : list_directory() )
                if ( ( f->dr & 0x7f ) == '?' || fcb_matches( f, n.c_str() ) )
                    g_search.push_back( n );
            result = search_next();
            break;
        }
        case 18: result = search_next(); break;
        case 19: // delete
        {
            Fcb * f = fcb_at( ds, dx );
            result = 0xff;
            for ( const string & n : list_directory() )
                if ( fcb_matches( f, n.c_str() ) && remove( n.c_str() ) == 0 )
                    result = 0;
            break;
        }
        case 20: // read sequential
        {
            Fcb * f = fcb_at( ds, dx );
            string name = fcb_file( f );
            uint32_t rec = seq_record( f );
            result = name.empty() ? 1 : read_record( name, rec );
            if ( result == 0 )
                set_seq_record( f, rec + 1 );
            if ( !name.empty() ) { uint8_t cr = f->cr; set_rc( f, name ); f->cr = cr; }
            break;
        }
        case 21: // write sequential
        {
            Fcb * f = fcb_at( ds, dx );
            string name = fcb_file( f );
            uint32_t rec = seq_record( f );
            result = name.empty() ? 2 : write_record( name, rec, false );
            if ( result == 0 )
                set_seq_record( f, rec + 1 );
            if ( !name.empty() ) { uint8_t cr = f->cr; set_rc( f, name ); f->cr = cr; }
            break;
        }
        case 22: // make
        {
            Fcb * f = fcb_at( ds, dx );
            if ( has_wildcards( f ) ) { result = 0xff; break; }
            string name = find_file( f );
            if ( name.empty() )
                name = fcb_name( f );
            FILE * fp = fopen( name.c_str(), "wb" );
            if ( !fp ) { if ( g_trace ) tracer.Trace( "  make %s failed\n", name.c_str() ); result = 0xff; break; }
            fclose( fp );
            remember_file( f, name );
            f->ex = f->s2 = f->rc = f->cr = 0;
            result = 0;
            break;
        }
        case 23: // rename: new name at FCB+16
        {
            Fcb * f = fcb_at( ds, dx );
            Fcb * to = fcb_at( ds, dx + 16 );
            string from = find_file( f );
            if ( from.empty() || !find_file( to ).empty() ) { result = 0xff; break; }
            result = rename( from.c_str(), fcb_name( to ).c_str() ) == 0 ? 0 : 0xff;
            break;
        }
        case 24: result = 1; break;                 // login vector: A:
        case 25: result = g_disk; break;
        case 26: g_dmaOff = dx; break;
        case 28: break;                             // write protect disk
        case 29: result = 0; break;                 // read-only vector
        case 30: result = find_file( fcb_at( ds, dx ) ).empty() ? 0xff : 0; break;
        case 32:
            if ( cpu.dl() == 0xff )
                result = g_user;
            else
                g_user = cpu.dl() & 0x0f;
            break;
        case 33: // read random
        case 34: // write random
        case 40: // write random with zero fill
        {
            Fcb * f = fcb_at( ds, dx );
            string name = fcb_file( f );
            if ( f->r2 & 0xfc ) { result = 6; break; }
            uint32_t rec = rand_record( f );
            if ( name.empty() ) { result = fn == 33 ? 4 : 5; break; }
            set_seq_record( f, rec );
            if ( fn == 33 )
                result = read_record( name, rec ) ? 1 : 0;
            else
                result = write_record( name, rec, fn == 40 );
            uint8_t cr = f->cr; set_rc( f, name ); f->cr = cr;
            break;
        }
        case 35: // compute file size
        {
            Fcb * f = fcb_at( ds, dx );
            string name = find_file( f );
            if ( name.empty() ) { result = 0xff; break; }
            long size = file_size( name );
            set_rand_record( f, (uint32_t) ( ( size + 127 ) / 128 ) );
            result = 0;
            break;
        }
        case 36: // set random record
        {
            Fcb * f = fcb_at( ds, dx );
            set_rand_record( f, seq_record( f ) );
            break;
        }
        case 37: result = 0; break;                 // reset drive
        case 51: g_dmaSeg = dx; break;
        case 52: // get DMA: ES:BX
            cpu.set_es( g_dmaSeg );
            cpu.set_ax( g_dmaOff );
            cpu.set_bx( g_dmaOff );
            setResult = false;
            break;
        case 53: result = get_max_mem( ds, dx, false, true ); break;
        case 54: result = get_max_mem( ds, dx, false, true ); break;
        case 55: result = get_max_mem( ds, dx, true, false ); break;
        case 56: result = get_max_mem( ds, dx, true, false ); break;
        case 57: case 58: result = 0; break;        // free memory: kept
        case 108: // CP/M-86 Plus: get/set program return code
            if ( dx == 0xffff )
                result = g_returnCode;
            else
                g_returnCode = dx;
            break;
        default:
            if ( g_trace )
                tracer.Trace( "unimplemented bdos function %u\n", fn );
            result = 0;
            break;
    }

    if ( setResult )
    {
        cpu.set_ax( result );
        cpu.set_bx( result );
    }
}

void i8086_invoke_syscall( uint8_t interrupt )
{
    if ( interrupt == 0xe0 )
        bdos();
    else if ( interrupt == 0xff ) // the program returned to the CCP
    {
        g_done = true;
        cpu.end_emulation();
    }
    else
        i8086_hard_exit( "unexpected syscall\n" );
}

// ---------------------------------------------------------------------------
// Loading

struct Group
{
    uint8_t type;
    uint16_t length, base, min, max;
    uint16_t seg;     // where it was loaded
    uint16_t size;    // paragraphs allocated
};

static void parse_fcb_arg( uint8_t * fcb, const string & arg )
{
    memset( fcb, 0, 16 );
    memset( fcb + 1, ' ', 11 );
    string a = arg;
    if ( a.size() >= 2 && a[ 1 ] == ':' )
    {
        fcb[ 0 ] = (uint8_t) ( toupper( a[ 0 ] ) - 'A' + 1 );
        a = a.substr( 2 );
    }
    size_t i = 0, j = 0;
    for ( ; i < a.size() && a[ i ] != '.' && j < 8; i++ )
    {
        if ( a[ i ] == '*' ) { while ( j < 8 ) fcb[ 1 + j++ ] = '?'; }
        else fcb[ 1 + j++ ] = (uint8_t) toupper( a[ i ] );
    }
    while ( i < a.size() && a[ i ] != '.' ) i++;
    if ( i < a.size() && a[ i ] == '.' )
    {
        i++;
        for ( j = 0; i < a.size() && j < 3; i++ )
        {
            if ( a[ i ] == '*' ) { while ( j < 3 ) fcb[ 9 + j++ ] = '?'; }
            else fcb[ 9 + j++ ] = (uint8_t) toupper( a[ i ] );
        }
    }
}

static void usage()
{
    fprintf( stderr, "usage: cpm86run [-t] [-e] [-m] program[.cmd] [arguments]\n" );
    exit( 2 );
}

int main( int argc, char * argv[] )
{
    bool useReturnCode = false, showMap = false;
    int a = 1;
    for ( ; a < argc && argv[ a ][ 0 ] == '-'; a++ )
    {
        if ( !strcmp( argv[ a ], "-t" ) ) g_trace = true;
        else if ( !strcmp( argv[ a ], "-e" ) ) useReturnCode = true;
        else if ( !strcmp( argv[ a ], "-m" ) ) showMap = true;
        else usage();
    }
    if ( a >= argc )
        usage();

    string path = argv[ a++ ];
    FILE * fp = fopen( path.c_str(), "rb" );
    if ( !fp && path.find( '.' ) == string::npos )
    {
        path += ".cmd";
        fp = fopen( path.c_str(), "rb" );
        if ( !fp ) { path = path.substr( 0, path.size() - 4 ) + ".CMD"; fp = fopen( path.c_str(), "rb" ); }
    }
    if ( !fp )
    {
        fprintf( stderr, "cpm86run: cannot open %s\n", argv[ a - 1 ] );
        return 2;
    }
    vector<uint8_t> file;
    {
        uint8_t buf[ 4096 ];
        size_t n;
        while ( ( n = fread( buf, 1, sizeof( buf ), fp ) ) > 0 )
            file.insert( file.end(), buf, buf + n );
        fclose( fp );
    }
    if ( file.size() < 128 )
    {
        fprintf( stderr, "cpm86run: %s: no CMD header\n", path.c_str() );
        return 2;
    }

    if ( g_trace )
    {
        tracer.Enable( true, "cpm86run.log", true );
        tracer.SetQuiet( true );
    }

    // The group descriptors, and the image of each group in turn.
    vector<Group> groups;
    for ( int i = 0; i < 8; i++ )
    {
        const uint8_t * d = &file[ i * 9 ];
        if ( d[ 0 ] == 0 )
            break;
        Group g = {};
        g.type = d[ 0 ] & 0x0f;
        g.length = d[ 1 ] | ( d[ 2 ] << 8 );
        g.base = d[ 3 ] | ( d[ 4 ] << 8 );
        g.min = d[ 5 ] | ( d[ 6 ] << 8 );
        g.max = d[ 7 ] | ( d[ 8 ] << 8 );
        if ( g.type < 1 || g.type > 9 )
        {
            fprintf( stderr, "cpm86run: %s: bad group type %u\n", path.c_str(), g.type );
            return 2;
        }
        groups.push_back( g );
    }
    if ( file[ 0x7f ] & 0x80 )
    {
        fprintf( stderr, "cpm86run: %s: segment fixups are not supported (CP/M-86 Plus)\n", path.c_str() );
        return 2;
    }

    size_t offset = 128;
    for ( Group & g : groups )
    {
        uint16_t need = g.min > g.length ? g.min : g.length;
        uint16_t want = g.max > need ? g.max : need;
        if ( g.base )
            g.seg = g.base;
        else
            g.seg = g_freeSeg;
        uint16_t avail = TpaEnd > g.seg ? TpaEnd - g.seg : 0;
        if ( need > avail )
        {
            fprintf( stderr, "cpm86run: %s: not enough memory\n", path.c_str() );
            return 2;
        }
        g.size = want < avail ? want : avail;
        // A group up to 64 KiB is what a 16-bit program can address; give
        // it no more than the maximum it asked for.
        size_t bytes = (size_t) g.length * 16;
        if ( offset + bytes > file.size() )
            bytes = offset < file.size() ? file.size() - offset : 0;
        memcpy( memory + ( (uint32_t) g.seg << 4 ), &file[ offset ], bytes );
        offset += (size_t) g.length * 16;
        if ( !g.base )
            g_freeSeg = g.seg + g.size;
    }

    Group * code = 0, * data = 0, * extra = 0, * stack = 0;
    Group * aux[ 4 ] = { 0, 0, 0, 0 };
    for ( Group & g : groups )
    {
        if ( ( g.type == 1 || g.type == 9 ) && !code ) code = &g;
        else if ( g.type == 2 && !data ) data = &g;
        else if ( g.type == 3 && !extra ) extra = &g;
        else if ( g.type == 4 && !stack ) stack = &g;
        else if ( g.type >= 5 && g.type <= 8 ) aux[ g.type - 5 ] = &g;
    }
    if ( !code )
    {
        fprintf( stderr, "cpm86run: %s: no code group\n", path.c_str() );
        return 2;
    }
    bool model8080 = !data;
    if ( !data )
        data = code;

    // The base page: the groups' last offsets and bases, the 8080 flag, the
    // FCBs of the first two arguments and the command tail.
    uint16_t bp = data->seg;
    memset( mem( bp, 0 ), 0, 256 );
    auto describe = [&]( uint16_t at, Group * g )
    {
        if ( !g )
            return;
        uint32_t last = (uint32_t) g->size * 16 - 1;
        *mem( bp, at ) = last & 0xff;
        *mem( bp, at + 1 ) = ( last >> 8 ) & 0xff;
        *mem( bp, at + 2 ) = ( last >> 16 ) & 0xff;
        wr16( bp, at + 3, g->seg );
    };
    describe( 0x00, code );
    *mem( bp, 0x05 ) = model8080 ? 1 : 0;
    describe( 0x06, data );
    describe( 0x0c, extra );
    describe( 0x12, stack );
    for ( int i = 0; i < 4; i++ )
        describe( 0x18 + 6 * i, aux[ i ] );

    string tail;
    vector<string> args;
    for ( ; a < argc; a++ )
    {
        args.push_back( argv[ a ] );
        tail += " ";
        for ( const char * p = argv[ a ]; *p; p++ )
            tail += (char) toupper( *p );
    }
    if ( tail.size() > 127 )
        tail.resize( 127 );
    parse_fcb_arg( mem( bp, 0x5c ), args.size() > 0 ? args[ 0 ] : "" );
    parse_fcb_arg( mem( bp, 0x6c ), args.size() > 1 ? args[ 1 ] : "" );
    *mem( bp, 0x80 ) = (uint8_t) tail.size();
    memcpy( mem( bp, 0x81 ), tail.data(), tail.size() );
    *mem( bp, 0x81 + tail.size() ) = 0;
    g_dmaSeg = bp;
    g_dmaOff = 0x80;

    // The BDOS interrupt and the exit stub.
    static const uint8_t bdosStub[] = { 0xcd, 0x69, 0xe0, 0xcf };      // int 69h; db 0e0h; iret
    static const uint8_t exitStub[] = { 0xcd, 0x69, 0xff, 0xf4 };      // int 69h; db 0ffh; hlt
    memcpy( mem( StubSeg, BdosStub ), bdosStub, sizeof( bdosStub ) );
    memcpy( mem( StubSeg, ExitStub ), exitStub, sizeof( exitStub ) );
    wr16( 0, 0xe0 * 4, BdosStub );
    wr16( 0, 0xe0 * 4 + 2, StubSeg );

    cpu.reset();
    cpu.enable_interrupt_syscall( true );
    cpu.set_interrupt( true );
    // The CCP calls the program with a far call on its own 96-byte stack.
    cpu.set_ss( CcpSeg );
    cpu.set_sp( CcpSp );
    cpu.push( StubSeg );
    cpu.push( ExitStub );
    cpu.set_cs( code->seg );
    cpu.set_ip( model8080 ? 0x100 : 0 );
    cpu.set_ds( data->seg );
    cpu.set_es( extra ? extra->seg : data->seg );

    if ( showMap )
    {
        fprintf( stderr, "cpm86run: %s model\n", model8080 ? "8080" : ( groups.size() == 2 && data != code ) ? "small" : "compact" );
        for ( Group & g : groups )
            fprintf( stderr, "  group %u at %04x, %04x paragraphs (file %04x, min %04x, max %04x)\n",
                     g.type, g.seg, g.size, g.length, g.min, g.max );
    }

    cpu.trace_instructions( g_trace );
    while ( !g_done )
        cpu.emulate( 1000000 );
    fflush( stdout );

    if ( useReturnCode )
        return g_returnCode & 0xff;
    return 0;
}
