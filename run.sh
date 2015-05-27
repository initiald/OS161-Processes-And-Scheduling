cd src
./configure --ostree=$HOME/cscc69/root

cd kern/conf
./config ASST1

cd ../compile/ASST1
bmake depend
bmake

bmake install

cd ../../..
bmake
bmake install

