# XMU Dumper.
Dump a raw image of a Original Xbox Memory Unit (XMU) straight from the console. Useful for backing up your XMU or using it on an emulator like xemu.

* See https://xboxdevwiki.net/Xbox_Memory_Unit

## Compile
Setup and install [nxdk](https://github.com/XboxDev/nxdk):
```
run the activate binary in `nxdk/bin/activate`
git clone https://github.com/Ryzee119/XMU_Dumper.git
cd XMU_Dumper
make
```

## Usage
* Copy the `default.xbe` to a folder on your xbox and launch it. See [releases](https://github.com/Ryzee119/XMU_Dumper/releases).
* Plug in a XMU into your controller slot
* The file file will be written to the same directory as the xbe.
