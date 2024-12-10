# AI-deck examples repository

Check out the [documentation](https://www.bitcraze.io/documentation/repository/aideck-gap8-examples/master/)
for starting guides. 

sudo docker run --rm -v ${PWD}:/module aideck-with-autotiler tools/build/make-example examples/other/hello_world_gap8 image

python3 -m cfloader flash examples/other/hello_world_gap8/BUILD/GAP8_V2/GCC_RISCV_FREERTOS/target.board.devices.flash.img deck-bcAI:gap8-fw -w radio://0/80/2M/E7E7E7E70D