# Host (Linux) AmiPart - image-file mode. Plain gcc, no dependencies.
CORE = cli script mountlist imagecopy partclone shrinkinfo ffsresize \
       sfsresize pfsresize partmove mbr build rdb clib sfs_util
SRCS = $(addprefix ../src/,$(addsuffix .c,$(CORE))) \
       amiga_shim.c host_rdargs.c host_locale.c host_main.c
amipart: $(SRCS) amiga_compat.h
	gcc -O2 -Wall -I . -I inc -I ../src -o amipart $(SRCS)
clean:
	rm -f amipart
