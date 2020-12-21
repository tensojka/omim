# MAPS.ME Original Project

This project is aimed at giving users of the MAPS.ME app a way to keep using the app they love even after version 2.0  lands in all app stores. [Short writeup](https://telegra.ph/What-happened-to-the-old-MAPSME-12-20) on context and origin.

Current upstream: [old organization repository](https://github.com/mapsme/omim). New development will be merged here from time to time.

## Communication

[Newsletter](https://mapsme.substack.com/)

[Telegram channel](https://t.me/originalmapsme)

[Telegram group](https://t.me/originalmapsmegroup)

## Short term goals

- ship working binary for Android and iOS
- move off official backend to be ready for it to get shut down


## Submodules

This repository contains submodules. Clone it with `git clone --recursive`. If you forgot,
run `git submodule update --init --recursive`.

## Translations

If you want to improve app translations or add more search synonyms, please check our [wiki](https://github.com/mapsme/omim/wiki).

## Compilation

To compile the project, you would need to initialize private key files. Run
`configure.sh` and press Enter to create empty files, good enough to build desktop
and Android debug packages.

For detailed installation instructions and Android/iOS building process,
see [INSTALL.md](https://github.com/mapsme/omim/tree/master/docs/INSTALL.md).

## Building maps

To create one or many map files, first build the project, then use python module [maps_generator](https://github.com/mapsme/omim/tree/master/tools/python/maps_generator).

## Map styles

MAPS.ME uses its own binary format for map styles, `drules_proto.bin`, which is compiled from
[MapCSS](https://wiki.openstreetmap.org/wiki/MapCSS) using modified Kothic library.
Feature set in MWM files depends on a compiled style, so make sure to rebuild maps after
releasing a style.

For development, use MAPS.ME Designer app along with its generator tool: these allow
for quick rebuilding of a style and symbols, and for producing a zoom-independent
feature set in MWM files.

See [STYLES.md](https://github.com/mapsme/omim/tree/master/docs/STYLES.md) for the
format description, instructions on building a style and some links.

## Development

You would need Qt 5 for development, most other libraries are included into the
repository: see `3party` directory. The team uses mostly XCode and Qt Creator,
though these are not mandatory. We have an established
[c++ coding style](https://github.com/mapsme/omim/blob/master/docs/CPP_STYLE.md) and [Objective-C coding style](https://github.com/mapsme/omim/blob/master/docs/OBJC_STYLE.md).

See [CONTRIBUTING.md](https://github.com/mapsme/omim/blob/master/docs/CONTRIBUTING.md)
for the repository initialization process, the description of all the directories
of this repository and other development-related information.

All contributors must sign a [Contributor Agreement](https://github.com/mapsme/omim/blob/master/docs/CLA.md),
so both our and their rights are protected.

## Feedback

Please report bugs and suggestions to [the issue tracker](https://github.com/mapsme/omim/issues),
or by mail to bugs@maps.me.

## Authors and License

This source code is Copyright (C) 2020 My.com B.V. (Mail.Ru Group), published under Apache Public License 2.0,
except third-party libraries. See [NOTICE](https://github.com/mapsme/omim/blob/master/NOTICE)
and [data/copyright.html](http://htmlpreview.github.io/?https://github.com/mapsme/omim/blob/master/data/copyright.html) files for more information.
