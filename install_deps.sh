#!/bin/bash
echo "正在安装依赖..."
sudo apt update
sudo apt install -y libboost-all-dev qtbase5-dev
echo "安装完成。"