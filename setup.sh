mkdir -p ~/.lenzhound
mkdir -p ~/.lenzhound/build

UNAME_STR=`uname`
if [ "$UNAME_STR" == 'Darwin' ]; then
	curl https://downloads.arduino.cc/arduino-1.8.1-macosx.zip > ~/.lenzhound/arduino.zip
	unzip ~/.lenzhound/arduino.zip -d ~/.lenzhound
elif [ "$UNAME_STR" == 'Linux' ];
	curl https://downloads.arduino.cc/arduino-1.8.1-linux64.tar.xz > ~/.lenzhound/arduino.tar.xz
	tar -xf ~/.lenzhound/arduino.tar.xz ~/.lenzhound/arduino
else
	echo "Unkown platform $UNAME_STR" 1>&2
	exit 1
fi

echo hello
