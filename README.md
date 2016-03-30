# opendiapason

OpenDiapason is a sampling engine designed for modelling pipe organs.

I apologise for the state of this repository. It probably is not ready for serious use and requires lots of work to make it user friendly.

## Getting things up and running.

On Linux, you will need portaudio, portmidi and cmake. On Ubuntu this is as easy as:

sudo apt-get install portaudio19-dev cmake libportmidi-dev

You will need "cop" and "fftset" as well (and "svgplot" if you're wanting to generate the interpolation filters):

git clone https://github.com/nickappleton/cop
git clone https://github.com/nickappleton/fftset
git clone https://github.com/nickappleton/opendiapason

Anything that is buildable as an application has a subdirectory beginning with "app". Building one of these is also easy. I suggest making a sub-directory for the build, run "CMake .." (if you want performance, make sure to build a release build using "-DCMAKE_BUILD_TYPE=Release").
