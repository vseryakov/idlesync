CFLAGS= -g -O3 -prebind -mmacosx-version-min=10.4 -mtune=pentium -arch i386
LIBS= -framework IOKit -framework CoreFoundation -framework ApplicationServices
APP=idlesync

all: $(APP)

$(APP): $(APP).cpp
	$(CXX) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f *.o $(APP)
