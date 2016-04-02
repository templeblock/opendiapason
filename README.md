# OpenDiapason

OpenDiapason is a sampling engine designed for modelling pipe organs.

I apologise for the state of this repository. It probably is not ready for serious use and requires lots of work to make it user friendly.

## Getting things up and running.

I do all of my development on OS X, but will try my best to keep everything working on Linux (Ubuntu) and Windows (10) as well. At some point I may set up some regression, but at the moment I'm focussing on getting something running that is useful. If you find that a Linux or Windows build is broken, let me know and I will try to fix it. If you find that a Linux or Windows build doesn't perform very well, I may not be able to help you - but if you fix it, please send me patches. :)

You will need my "cop" and "fftset" repositories regardless of the operating system you are using and everything must reside at the same level in your filesystem. If you intend on fiddling around with the interpolation filter (which you really do not need to do), you will also need the "svgplot" repository.

> git clone https://github.com/nickappleton/opendiapason
git clone https://github.com/nickappleton/cop
git clone https://github.com/nickappleton/fftset

I follow a convention in my repositories that anything buildable as an application resides in a subdirectory beginning with "app" (i.e. this directory always contains a CMakeLists.txt file containing a project() directive). You should always be able to point cmake at one of these directories to get project files for whatever that application is for.

### OS X

- You need cmake.
- You need portaudio and portmidi.
- If the portaudio and portmidi libraries/headers are in usual locations on the system, you should just be able to use cmake to create Makefiles and build any of the applications.

### Linux

- You need cmake.
- You need portaudio and portmidi.
-- On Ubuntu, this is as easy as `sudo apt-get install portaudio19-dev cmake libportmidi-dev`
- You should just be able to use cmake to create Makefiles and build any of the applications.

### Windows

On Windows, things are a little bit trickier. I am using MS Visual Studio 2015 Community Edition (as Visual Studio seems like the right thing to do on Windows). You will need to get and build portaudio and portmidi (there is some tricky-ness here) and you will need to build a 64-bit binary... this is mainly to do with not finding vector types in 32-bit so the FFT will not build - there is probably a way to get around this but I haven't bothered to look into it. If you do, please update this readme. :)

You will need to specify the locations of where cmake should search for the portaudio/midi headers. My cmake scripts will look for the variables PORTAUDIO_INCLUDEDIR, PORTAUDIO_LIBDIR to search, so the command line will end up looking something like this:

`cmake .. -G "Visual Studio 14 2015 Win64" -DPORTAUDIO_LIBDIR=C:\things\portaudiobin\Release -DPORTAUDIO_INCLUDEDIR=C:\things\port2\include`

You will also need to copy the built portaudio library into the build directory in order to run the binary.