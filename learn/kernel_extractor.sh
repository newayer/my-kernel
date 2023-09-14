#!/bin/bash

ROOT_DIR=`pwd`
OBJ_DIR=$1

cd $OBJ_DIR
OBJ_DIR=`pwd`

find -name *.o.cmd | grep -v built-in.o.cmd | grep -v mod.o.cmd |
   grep -v -F "./scripts/" | grep -v -F "./tools/" | grep -v -F "./..tmp_kallsyms" > $ROOT_DIR/tmp1.txt

find -name *.dtb.cmd >> $ROOT_DIR/tmp1.txt

cat `cat $ROOT_DIR/tmp1.txt`| sort -u > $ROOT_DIR/tmp2.txt

sed -n "s@^source_.* := \(.*\)@\1@p" $ROOT_DIR/tmp2.txt > $ROOT_DIR/tmpc.txt
grep -v "wildcard include/config" $ROOT_DIR/tmp2.txt | grep -v ":" |
   sed -n 's@ *\(.*\) *\\$@\1@p' > $ROOT_DIR/tmph.txt
echo include/generated/autoconf.h >> $ROOT_DIR/tmph.txt

cat $ROOT_DIR/tmpc.txt $ROOT_DIR/tmph.txt > $ROOT_DIR/tmp3.txt

sed -e 's@\([^\./]*\)/\.\./@@g' $ROOT_DIR/tmp3.txt > $ROOT_DIR/tmp4.txt
for i in 1 2 3 4 5 6
do
    sed -e 's@/\([^\./]*\)/\.\.@@g' $ROOT_DIR/tmp4.txt > $ROOT_DIR/tmpd.txt
    mv $ROOT_DIR/tmpd.txt $ROOT_DIR/tmp4.txt
done

sed -e "s@^[^/].*@${OBJ_DIR}/&@" $ROOT_DIR/tmp4.txt > $ROOT_DIR/tmp5.txt

sed -e "s@$OBJ_DIR/@@g" $ROOT_DIR/tmp5.txt | sort -u > $ROOT_DIR/result.txt

tar -cvf $ROOT_DIR/kernel_learn.tar .config `grep -v -E "^/" $ROOT_DIR/result.txt`

cd -

tar -xvf $ROOT_DIR/kernel_learn.tar

for file in `grep -E "^/" result.txt`
do
    cp $file include/
done

src_dir=`grep kernel/printk/printk.c result.txt | sed -e "s@/\(.*\)/kernel/printk/printk\.c@\1@"`
if [ "$src_dir" != "kernel/printk/printk.c" ];
then
    cd $src_dir
    cp -r * $ROOT_DIR/
    cd -
    rm -fr ${src_dir%%/*}
    cp -r /$src_dir/include/asm-generic/* include/asm-generic/
    cp /$src_dir/include/linux/kconfig.h include/linux/
else
    cp -r /$OBJ_DIR/include/asm-generic/* include/asm-generic/
    cp /$OBJ_DIR/include/linux/kconfig.h include/linux/
fi

rm -fr $ROOT_DIR/kernel_learn.tar $ROOT_DIR/tmp*.txt $ROOT_DIR/result.txt

echo "begin to ln extracted files......"

for line in `find -type f -name "*.[c|h|S|d]*"`
do
	dir=$(dirname $line)/
	lnfile=`echo $dir | sed 's@[^/]*/@../@g'``echo $line | sed 's@\.\/@@'`

    rm -fr $line && ln -s $lnfile $line
done
