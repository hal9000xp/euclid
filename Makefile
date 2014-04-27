files = `find -maxdepth 2 -name '*.c'|grep -v '\<calc_crc\.c\>'|sed 's/\.\///'`

includes = `find -maxdepth 1 -type d | egrep -v '\.git' | \
		   xargs -I % echo "-I"% | xargs`

all: module_list gitrev

	gcc -DDEBUG -O0 -Wall -Werror -g $(includes) -o euclid $(files)	\
		-lssl -lcrypto -lz

test_many_conns: module_list gitrev

	gcc -DDEBUGMANYCONNS -DDEBUG -O0 -Wall -Werror -g $(includes)	\
		-o euclid $(files) -lssl -lcrypto -lz

release: module_list gitrev

	gcc -O2 -Wall -Werror -g $(includes) -o euclid $(files) 		\
		-lssl -lcrypto -lz

module_list:
	echo "#include \"main.h\"" > module_list.c
	echo "#include \"linked_list.h\"" >> module_list.c
	echo "#include \"module.h\"" >> module_list.c
	echo "#include \"module_list.h\"" >> module_list.c
	find -maxdepth 2 -name '*.module' | xargs -n 1 head -1 >> module_list.c
	echo "" >> module_list.c
	echo "void module_list_init()" >> module_list.c
	echo "{" >> module_list.c
	find -maxdepth 2 -name '*.module' | xargs -n 1 tail -1 >> module_list.c
	echo "}" >> module_list.c

gitrev:
	echo "#include \"gitrev.h\"" > gitrev.c
	echo "" >> gitrev.c
	echo "const char *G_gitrev = \""`git rev-parse --verify HEAD`"\";"	\
		>> gitrev.c

crc:
	gcc -Wall -Werror -g -o calc_crc core/calc_crc.c core/crc32.c

clean:
	rm -f euclid calc_crc gitrev.c module_list.c tags

