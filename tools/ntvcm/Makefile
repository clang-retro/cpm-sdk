# Build ntvcm with the host C++ compiler.
CXX ?= c++
CXXFLAGS ?= -O2
SRCS = ntvcm.cxx x80.cxx
HDRS = x80.hxx ntvcm.h djl_con.hxx djl_cycle.hxx djl_os.hxx djl_perf.hxx \
       djl_rssrdr.hxx djltrace.hxx
ntvcm: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -std=c++17 -fno-builtin -DNDEBUG -I. -o $@ $(SRCS)
clean:
	rm -f ntvcm
