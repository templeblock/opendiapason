# OpenDiapason

OpenDiapason is a sampling engine designed for modelling pipe organs.

I apologise for the state of this repository. It probably is not ready for serious use and requires lots of work to make it user friendly.

## Getting things up and running.

I do all of my development on OS X, but will try my best to keep everything working on Linux (Ubuntu) and Windows (10) as well. At some point I may set up some regression, but at the moment I'm focussing on getting something running that is useful. If you find that a Linux or Windows build is broken, let me know and I will try to fix it. If you find that a Linux or Windows build doesn't perform very well, I may not be able to help you - but if you fix it, please send me patches. :)

You will need my "cop" and "fftset" repositories regardless of the operating system you are using and everything must reside at the same level in your filesystem. If you intend on fiddling around with the interpolation filter (which you really do not need to do), you will also need the "svgplot" repository.

> git clone https://github.com/nickappleton/opendiapason  
> git clone https://github.com/nickappleton/cop  
> git clone https://github.com/nickappleton/fftset

I follow a convention in my repositories that anything buildable as an application resides in a subdirectory beginning with "app" (i.e. this directory always contains a CMakeLists.txt file containing a project() directive). You should always be able to point CMake at one of these directories to get project files for whatever that application is for.

OpenDiapason uses PortAudio and PortMidi for audio/midi support. On linux and OS X, if you have PortAudio installed on your system, CMake will try to find the libraries and headers. If you do not have PortAudio installed in a usual place, you will need to specify the PORTAUDIO_INCLUDEDIR, PORTAUDIO_LIBDIR, PORTMIDI_INCLUDEDIR and PORTMIDI_LIBDIR CMake variables. On Windows, PortAudio can be built alongside OpenDiapason in the same way as cop and fftset. i.e.

> git clone https://git.assembla.com/portaudio.git

If the build system finds a "portaudio" directory at the same level as the other dependencies, it will automatically include it when building OpenDiapason. This makes things easier on Windows systems. Unfortunately, portmidi is a bit more complex to get up and running on Windows and I usually need to hack it just to get it to compile.

On Linux, getting PortAudio/PortMidi development headers is as easy as:

> sudo apt-get install portaudio19-dev cmake libportmidi-dev

### Notes for Windows

I am using MS Visual Studio 2015 Community Edition (as Visual Studio seems like the right thing to do on Windows). The simplest way to get PortAudio working is to clone it at the same level as the other dependencies as mentioned previously. PortMidi requires more attention and it's never compiled for me out of the box - if you hack out JNI support, things generally start working - but there isn't a clean way to disable it using the build system.

As mentioned above, once you have a libraries built, you must specify the PORTMIDI_INCLUDEDIR and PORTMIDI_LIBDIR (and PORTAUDIO_INCLUDEDIR and PORTAUDIO_LIBDIR if you built it too from scratch).

### Current issues

I've really only noticed big problems on Windows. The main issue at the moment is related to compressed memory kicking in and starting to move chunks of the loaded samples somewhere else. After running for two days, the memory usage with one particular sample set went from 4 GB to 2 MB - as soon as keys start playing, theres glitching everywhere as everything gets uncompressed again. I think there are some hacky ways to solve this (a thread that continously reads from the allocated memory), but I'm not doing that until I'm convinced that I'm not missing something on Windows... ping me if you've got any ideas
