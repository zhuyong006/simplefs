cmd_drivers/demo/hook/hook.ko := arm-linux-gnueabi-ld -EL -r  -T ./scripts/module-common.lds --build-id  -o drivers/demo/hook/hook.ko drivers/demo/hook/hook.o drivers/demo/hook/hook.mod.o
