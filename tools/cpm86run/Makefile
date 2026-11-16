# Build cpm86run with the host C++ compiler.
CXX ?= c++
CXXFLAGS ?= -O2
cpm86run: cpm86run.cxx i8086.cxx i8086.hxx djl8086d.hxx djltrace.hxx djl_os.hxx
	$(CXX) $(CXXFLAGS) -std=c++17 -fno-builtin -DNDEBUG -I. -o $@ cpm86run.cxx i8086.cxx
clean:
	rm -f cpm86run
