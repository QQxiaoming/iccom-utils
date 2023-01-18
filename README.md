[![CI](https://github.com/QQxiaoming/iccom-utils/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/QQxiaoming/iccom-utils/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/qqxiaoming/iccom-utils.svg?colorB=f48041&style=flat-square)](https://github.com/QQxiaoming/iccom-utils)

# iccom-utils

English | [简体中文](./README_zh_CN.md)

## introduction

This project is a practical tool for writing on the spi symmetrical full-duplex socket communication protocol stack using [linux-symspi](https://github.com/Bosch-SW/linux-symspi),[linux-iccom](https://github.com/Bosch-SW/linux-iccom) and [libiccom](https://github.com/Bosch-SW/libiccom) projects, including:

- iccshd/iccsh/icccp
- iccom_send
- iccom_recv

> 1.The source files under the driver path in the project warehouse have some bugfixes and optimization improvements based on the original project linux-symspi/linux-iccom. The code respects the source license, but the version of the original project can still be used to cooperate with iccom-util. tool use.<br>2.The source files under the lib path in the project warehouse have some bugfixes and optimization improvements based on the original project libiccom. The code respects the source license, but the version of the original project can still be used with the iccom-util utility tool.

### iccshd/iccsh/icccp

iccsh is a terminal forwarding tool based on the iccom transport protocol. The host host runs iccshd, and the client host runs iccsh to perform terminal operations through the spi-based physical layer (The design idea is similar to the sshd/ssh mode, but the currently implemented iccsh does not include encryption). icccp is used for file copying (the design idea is similar to scp).

### iccom_send

iccom_send is a practical tool for sending data under the iccom transmission protocol (the design idea is similar to the can-utils series), and the usage is as follows:

```shell
iccom_send - send iccom-frames via sockets.
Usage: iccom_send <frame>.
    <frame>:
        <ch_id>#{data}: for iccom data frames
            <ch_id>:
                2 byte hex chars
            {data}:
                ASCII hex-values
Examples:
    iccom_send 15A1#1122334455667788
```

### iccom_recv

iccom_recv is a practical tool for data reception under the iccom transmission protocol (the design idea is similar to the can-utils series), and the usage is as follows:

```shell
iccom_recv - recv iccom-frames via sockets.
Usage: iccom_recv <ch_id>.
    <ch_id>:
        2 byte hex chars
Examples:
    iccom_recv 15A1
```

## TODO

- [ ] iccsh support encryption
- [ ] iccshd support custom port num
- [ ] iccshd support Multi-terminal connection
- [ ] icccp support recursive
- [ ] icccp improve the way the target path is a directory
