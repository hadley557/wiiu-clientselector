Wii U Server Selector (Fork)

## IMPORTANT NOTICE:

This is a fork of wiiu-serverselector, the network download location is different.
located in sd:/wiiu/wiiu-clients/{NetworkName}/ ---> {filename}.wps and {filename}.wms
you may download Protarium Network and Pretendo Network directly from the plugin.
if you have issues contact me in the Protarium Network Discord Server!

## Don't follow this btw:
it's old instructions i just wanted to keep it for the original dev of wiiu-serverselector.

Lets you swap between different Wii U servers (ex. Pretendo, Protarium) with a menu in the home menu. Make a folder in the root of your SD Card called "wiiu-plugins" and put your .wms and .wps files into a folder inside the "wiiu-plugins" folder, directly into that folder. Still a work in progress, built off of the "evWii" plugin code.
  
note: if anything crashes, try deleting any "inkay-pretendo.wms" and "inkay-pretendo.wps" files, or whatever they may be called for you.


HOW TO USE
1. drop wiiuserverselector.wps into sd:/wiiu/environments/aroma/plugins/
2. make a folder on the root of your sd card called "wiiu-servers"
3. pretty much every online server uses a .wps and .wms file. for example, pretendo uses inkay-pretendo.wps and inkay-pretendo.wps. for whatever server you're trying to load, find the .wps and .wms file and make a folder in the "wiiu-servers" folder named your server name and put those files in the folder. your end result should look like SD:/wiiu-servers/[servername]/[filename].wps and SD:/wiiu-servers/[servername]/[filename].wms

tip: reboot your wii u to see newly added servers 

here's an example of what your folder should look like
<img width="2880" height="1800" alt="Screenshot 2026-09-08 233231" src="https://github.com/user-attachments/assets/ba1cb070-673d-4dce-b3bb-30a17d961fdf" />

HOW TO COMPILE
1. download the docker desktop app
2. run these two lines of code
"docker build -t evwii_builder ."
"docker run --rm -v ${PWD}:/project evwii_builder make"
3. done
