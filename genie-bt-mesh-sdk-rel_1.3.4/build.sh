#!/bin/bash

##------default config-----------------------------------------------------------------------------------##
default_app="bluetooth.light_ctl"
default_board="tg7100b"
default_debug=1
default_args=""

#args eg: 
#GENIE_GPIO_UART=1
#GENIE_MESH_AT_CMD=1
#GENIE_MESH_BINARY_CMD=1
#GENIE_MESH_GLP=1
#

##-------------------------------------------------------------------------------------------------------##

start_time=$(date +%s)

if [ "$(uname)" = "Linux" ]; then
    CUR_OS="Linux"
elif [ "$(uname)" = "Darwin" ]; then
    CUR_OS="OSX"
elif [ "$(uname | grep NT)" != "" ]; then
    CUR_OS="Windows"
else
    echo "error: unkonw OS"
    exit 1
fi

if [[ xx"$1" == xxclean ]]; then
	rm -rf out
	exit 0
fi

if [[ xx"$1" == xx--help ]] || [[ xx"$1" == xxhelp ]] || [[ xx"$1" == xx-h ]] || [[ xx"$1" == xx-help ]]; then
	echo "./build.sh $default_app $default_board $default_debug $default_args "
	exit 0
fi

app=$1
if [ xx"$1" == xx ]; then
	app=$default_app
fi

board=$2
if [ xx"$2" == xx ]; then
	board=$default_board
fi

if [[ xx$3 == xx ]]; then 
	echo "CONFIG_DEBUG_MODE SET AS 1"
	CONFIG_DEBUG_MODE=$default_debug
else
	CONFIG_DEBUG_MODE=$3
fi

if [[ xx$4 == xx ]]; then 
	echo "ARGS SET AS NULL"
	ARGS=$default_args
else
	ARGS="$4"
fi

echo "----------------------------------------------------------------------------------------------------"
echo "OS: ${CUR_OS}"
echo "app_name=$app board=$board CONFIG_DEBUG_MODE=$CONFIG_DEBUG_MODE ARGS=$ARGS"
echo "./build.sh $app $board $CONFIG_DEBUG_MODE $ARGS"
echo "----------------------------------------------------------------------------------------------------"

function build_end()
{
	end_time=$(date +%s)
	cost_time=$[ $end_time-$start_time ]
	echo "build time is $(($cost_time/60))min $(($cost_time%60))s"
}

mkdir -p out;mkdir -p out/$app@${board}
rm -rf out/$app@${board}/*
aos make clean
echo -e "aos make $app@${board} CONFIG_DEBUG_MODE=${CONFIG_DEBUG_MODE} $ARGS"
aos make $app@${board} CONFIG_DEBUG_MODE=${CONFIG_DEBUG_MODE} "$ARGS"
if [[ -f out/$app@${board}/binary/total_image.hexf ]]; then
	build_end
	exit 0
else
	echo "build failed!"
	exit 1
fi

exit 0
