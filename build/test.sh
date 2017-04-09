#!/bin/bash

set -ev

useradd developer
mkdir /home/developer
chown -R developer:developer /home/developer

cd /home/developer
su - developer

git config --global user.email "testing-on-gitlab-ci@example.com"
git config --global user.name "CI Builder"

git clone git://github.com/droundy/bigbro
cd bigbro

cargo build --features strict
cargo test --features strict
cargo doc --features strict
# cargo build --features strict --target x86_64-apple-darwin
cargo build --features strict --target i686-pc-windows-gnu
cargo build --features strict --target x86_64-pc-windows-gnu

rm -rf target

sh build/linux.sh
python3 run-tests.py -v

# execute this with:
# docker build -t facio/bigbro .
# docker run --security-opt seccomp:../docker-security.json facio/bigbro bash test.sh
