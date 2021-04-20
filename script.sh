#!/bin/<shell>

cd "/media/chinmay/Academics/UTAustin/My code/Raman2/Galois/build/tools/graph-convert/"

echo -e "Please enter the root of the input file: "
read inputfile

echo -e "Please enter the number of subgraphs: "
read n

for ((number=0; number<$n; number++))
do
	./graph-convert "--gr2tgr" "$inputfile.gr.$number.of.$n" "$inputfile-transpose.gr.$number.of.$n"
	rm -r "$inputfile.gr.$number.of.$n"
	mv "$inputfile.gr-LtoG-$number" "$inputfile-transpose.gr-LtoG-$number"  
done





