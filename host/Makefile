# Host (Linux) AmiPart - image-file mode. Plain gcc, no dependencies.
CORE = cli script mountlist imagecopy partclone shrinkinfo ffsresize \
       sfsresize pfsresize partmove mbr build rdb clib sfs_util nativefmt ataident clearrdb rescue
SRCS = $(addprefix ../src/,$(addsuffix .c,$(CORE))) \
       amiga_shim.c host_rdargs.c host_locale.c host_devices.c host_main.c
amipart: $(SRCS) amiga_compat.h
	gcc -O2 -Wall -Wno-pointer-sign -Wno-format-security -Wno-stringop-truncation -DAMIPART_HOST=1 -I . -I inc -I ../src -o amipart $(SRCS)

# Linux GUI: the Amiga GUI source on an Intuition/GadTools layer over SDL2
# (host_gui.c, host_gadtools.c, host_req.c).  Needs libsdl2-dev.
# With arguments it is the same CLI as amipart; without, it opens the GUI.
GUI    = main diskselect guilv guisize partview partview_clear \
         partview_dialogs partview_diskcopy partview_fs partview_image \
         partview_mbr partview_move partview_partclone partview_partcopy \
         partview_rdb partview_shrink partview_zero progresswin
GSRCS  = $(addprefix ../src/,$(addsuffix .c,$(CORE) $(GUI))) \
         amiga_shim.c host_rdargs.c host_locale.c host_devices.c \
         host_gui.c host_gadtools.c host_req.c
GHDRS  = amiga_compat.h host_gui.h hg_int.h gui_font.h
SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)
GOBJ   = gobj
GOBJS  = $(addprefix $(GOBJ)/,$(notdir $(GSRCS:.c=.o)))
GFLAGS = -O2 -Wall -Wno-pointer-sign -Wno-format -Wno-format-security \
         -Wno-stringop-truncation -Wno-unused-function \
         -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast \
         -DAMIPART_HOST=1 -DAMIPART_HOST_GUI=1 -I . -I inc -I ../src $(SDL_CFLAGS)

gui: amipart-gui
amipart-gui: $(GOBJS)
	gcc -o $@ $(GOBJS) $(SDL_LIBS)
$(GOBJ)/%.o: ../src/%.c $(GHDRS) | $(GOBJ)
	@echo "  CC $<"; gcc $(GFLAGS) -c -o $@ $<
$(GOBJ)/%.o: %.c $(GHDRS) | $(GOBJ)
	@echo "  CC $<"; gcc $(GFLAGS) -c -o $@ $<
$(GOBJ):
	mkdir -p $(GOBJ)

clean:
	rm -f amipart amipart-gui
	rm -rf $(GOBJ)
.PHONY: gui clean
