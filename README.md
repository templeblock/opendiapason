# opendiapason

OpenDiapason is a sampling engine designed for modelling pipe organs.

I apologise for the state of this repository. It probably is not ready for serious use and requires lots of work to make it user friendly.

## Getting things up and running.

### Linux

On Linux, you will need portaudio, portmidi and cmake. On Ubuntu this is as easy as:

sudo apt-get install portaudio19-dev cmake libportmidi-dev

You will need "cop" and "fftset" as well (and "svgplot" if you're wanting to generate the interpolation filters):

git clone https://github.com/nickappleton/cop
git clone https://github.com/nickappleton/fftset
git clone https://github.com/nickappleton/opendiapason

Anything that is buildable as an application has a subdirectory beginning with "app". Building one of these is also easy. I suggest making a sub-directory for the build, run "CMake .." (if you want performance, make sure to build a release build using "-DCMAKE_BUILD_TYPE=Release").

### Windows

On Windows, things are a little bit trickier. I am using MS Visual Studio 2015 Community Edition (as that seems like the right thing to do on Windows). You will need to get portaudio and portmidi and build them (there is some tricky-ness here too) and you will need to build a 64-bit binary... this is mainly to do with not finding vector types so the FFT will not build - there is probably a way to get around this but I haven't bothered to look into it. If you do, please update this readme. :)

You will need to specify the locations of where cmake should search for the portaudio/midi headers. My cmake scripts will look for the variables PORTAUDIO_INCLUDEDIR, PORTAUDIO_LIBDIR to search, so the command line will end up looking something like this:

cmake .. -G "Visual Studio 14 2015 Win64" -DPORTAUDIO_LIBDIR=C:\things\portaudiobin\Release -DPORTAUDIO_INCLUDEDIR=C:\things\port2\include

You will also need to copy the built portaudio library into the build directory in order to run the binary.
