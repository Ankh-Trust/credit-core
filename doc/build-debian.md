# Debian 9 (Stretch) build guide for Credit

**NOTE: Lines ending in \ are part of a shell command that spreads over multiple
lines, these can be copypasted as a whole (as opposed to line-by-line).**

## Update the system

```
apt-get update
apt-get -y upgrade
apt-get -y dist-upgrade
```

## Essential build packages
```
apt-get install build-essential autoconf automake libtool pkg-config git
```

Reboot, autoconf for Credit might complain about pkg-config otherwise.

```
shutdown -r now
```

## Libraries needed for Credit
```
apt-get -y install libminiupnpc-dev libdb++-dev libboost-system-dev \
          libboost-filesystem-dev libboost-program-options-dev \
          libboost-thread-dev libboost-chrono-dev libssl1.0-dev libevent-dev
```

## Credit build

```
useradd -m -d /home/credit -s /bin/bash credit
su - credit
git clone https://github.com/duality-solutions/Credit src
cd src
./autogen.sh
./configure --disable-tests --disable-gui-tests --disable-bench \
              --disable-zmq --enable-experimental-asm --with-incompatible-bdb \
              --with-gui=no
```

**NOTE: BUILDING Credit WITH LESS THAN 2GB OF RAM DOES NOT WORK!!!**

On systems with very little RAM (2GB) this CXXFLAGS cheat should fix
memory-related build issues (i.e. Cannot allocate and the likes).

```
CXXFLAGS="--param ggc-min-expand=1 --param ggc-min-heapsize=32768" make
```

On systems with enough RAM (i.e. 4GB and up) just execute make:

```
make
```

## Local installation

On Debian as well as Ubuntu, ~/bin usually is in the PATH per default.

```
mkdir ~/bin
cp src/credit-cli src/credit-tx src/creditd ~/bin
strip ~/bin/credit*
```

## Base config for Credit wallet
```
mkdir ~/.credit
echo "txindex=1" >> ~/.credit/credit.conf
echo "listen=1" >> ~/.credit/credit.conf
echo "rpcuser=credit" >> ~/.credit/credit.conf
echo "rpcpassword=`head -c 32 /dev/urandom | base64`" >> ~/.credit/credit.conf
chmod 0700 ~/.credit
chmod 0600 ~/.credit/credit.conf
```

## Start creditd

Watch debug.log to see if theres any errors. Log display can be exited by pressing
`ctrl-c`.

```
~/bin/creditd -daemon
tail -f ~/.credit/debug.log
```

Check with http://_ac.blocksandchain.com/ that the block height is the same
as it is reported by the output of the below command. It is labeled 'blocks'
in the getinfo-output. If the numbers match, the installation is completed.

```
credit-cli getinfo
{
  "version": 1010000,
  "protocolversion": 71110,
  "walletversion": 100000,
  "balance": 0.00000000,
  "privatesend_balance": 0.00000000,
  "blocks": 193619,
  "timeoffset": 0,
  "connections": 16,
  "proxy": "127.0.0.1:9050",
  "difficulty": 1.129563238994795,
  "testnet": false,
  "keypoololdest": 1538953839,
  "keypoolsize": 1999,
  "paytxfee": 0.00000000,
  "relayfee": 0.00001000,
  "errors": ""
}

```
