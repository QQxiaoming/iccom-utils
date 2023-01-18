[![CI](https://github.com/QQxiaoming/iccom-utils/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/QQxiaoming/iccom-utils/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/qqxiaoming/iccom-utils.svg?colorB=f48041&style=flat-square)](https://github.com/QQxiaoming/iccom-utils)

# iccom-utils

[English](./README.md) | 简体中文

## 介绍

本项目是用于在使用[linux-symspi](https://github.com/Bosch-SW/linux-symspi),[linux-iccom](https://github.com/Bosch-SW/linux-iccom),[libiccom](https://github.com/Bosch-SW/libiccom)项目搭建spi对称全双工socket通信协议栈上编写的实用工具，包括：

- iccshd/iccsh/icccp
- iccom_send
- iccom_recv

> 1.本项目仓库内driver路径下源文件是在原项目linux-symspi/linux-iccom基础上进行了一定的bugfix和优化改进，代码尊重源License，但仍可以使用原项目的版本配合iccom-util实用工具使用. <br>2.本项目仓库内lib路径下源文件是在原项目libiccom基础上进行了一定的bugfix和优化改进，代码尊重源License，但仍可以使用原项目的版本配合iccom-util实用工具使用.

### iccshd/iccsh/icccp

iccsh是基于iccom传输协议的终端转发工具，host主机运行iccshd，客户端主机运行iccsh即可通过基于spi的物理层进行终端操作（设计思路类似sshd/ssh模式，但目前实现的iccsh不包含加密）。icccp用于文件拷贝（设计思路类似scp）。

### iccom_send

iccom_send是一个iccom传输协议下数据发送的实用工具（设计思路类似can-utils系列），使用方式如下：

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

iccom_recv是一个iccom传输协议下数据接收的实用工具（设计思路类似can-utils系列），使用方式如下：

```shell
iccom_recv - recv iccom-frames via sockets.
Usage: iccom_recv <ch_id>.
    <ch_id>:
        2 byte hex chars
Examples:
    iccom_recv 15A1
```
