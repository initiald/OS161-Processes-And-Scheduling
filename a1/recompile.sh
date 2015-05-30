cd kern/conf
./config ASST1
cd ../compile/ASST1
bmake depend
bmake
bmake install
cd ../../../../../root
sys161 kernel


