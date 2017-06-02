BINDIR = /usr/lib/clightd
BINNAME = clightd
BUSCONFDIR = /etc/dbus-1/system.d/
BUSCONFNAME = org.clightd.backlight.conf
BUSSERVICEDIR = /usr/share/dbus-1/system-services/
BUSSERVICENAME = org.clightd.backlight.service
SYSTEMDSERVICE = clightd.service
SYSTEMDDIR = /usr/lib/systemd/system
POLKITPOLICYNAME = org.clightd.backlight.policy
POLKITPOLICYDIR = /usr/share/polkit-1/actions
EXTRADIR = Scripts
DEBIANDIR = ./Clightd
DEBOUTPUTDIR = ./Debian
RM = rm -f
RMDIR = rm -rf
INSTALL = install -p
INSTALL_PROGRAM = $(INSTALL) -m755
INSTALL_DATA = $(INSTALL) -m644
INSTALL_DIR = $(INSTALL) -d
SRCDIR = src/
LIBS = -lm $(shell pkg-config --libs libsystemd libudev libdrm)
CFLAGS = $(shell pkg-config --cflags libsystemd libudev libdrm) -D_GNU_SOURCE -std=c99

ifeq (,$(findstring $(MAKECMDGOALS),"clean install uninstall"))

ifneq ("$(shell pkg-config --atleast-version=221 systemd && echo yes)", "yes")
$(error systemd minimum required version 221.)
endif

ifneq ("$(DISABLE_FRAME_CAPTURES)","1")
$(info Frames capturing support enabled.)
else
CFLAGS+=-DDISABLE_FRAME_CAPTURES
$(info Frames capturing support disabled.)
endif

endif

CLIGHTD_VERSION = $(shell git describe --abbrev=0 --always --tags)

all: clightd clean

debug: clightd-debug clean

objects:
	@cd $(SRCDIR); $(CC) -c *.c $(CFLAGS)

objects-debug:
	@cd $(SRCDIR); $(CC) -c *.c -Wall $(CFLAGS) -Wshadow -Wtype-limits -Wstrict-overflow -fno-strict-aliasing -Wformat -Wformat-security -g

clightd: objects
	@cd $(SRCDIR); $(CC) -o ../$(BINNAME) *.o $(LIBS)

clightd-debug: objects-debug
	@cd $(SRCDIR); $(CC) -o ../$(BINNAME) *.o $(LIBS)

clean:
	@cd $(SRCDIR); $(RM) *.o

deb: all install-deb build-deb clean-deb

install-deb: DESTDIR=$(DEBIANDIR)
install-deb: install

build-deb:
	$(info setting deb package version.)
	@sed -i 's/Version:.*/Version: $(CLIGHTD_VERSION)/' ./DEBIAN/control
	$(info copying debian build script.)
	@cp -r DEBIAN/ $(DEBIANDIR)
	@$(INSTALL_DIR) $(DEBOUTPUTDIR)
	$(info creating debian package.)
	@dpkg-deb --build $(DEBIANDIR) $(DEBOUTPUTDIR)

clean-deb:
	$(info cleaning up.)
	@$(RMDIR) $(DEBIANDIR)

install:
	$(info installing bin.)
	@$(INSTALL_DIR) "$(DESTDIR)$(BINDIR)"
	@$(INSTALL_PROGRAM) $(BINNAME) "$(DESTDIR)$(BINDIR)"

	$(info installing dbus conf file.)
	@$(INSTALL_DIR) "$(DESTDIR)$(BUSCONFDIR)"
	@$(INSTALL_DATA) $(EXTRADIR)/$(BUSCONFNAME) "$(DESTDIR)$(BUSCONFDIR)"

	$(info installing dbus service file.)
	@$(INSTALL_DIR) "$(DESTDIR)$(BUSSERVICEDIR)"
	@$(INSTALL_DATA) $(EXTRADIR)/$(BUSSERVICENAME) "$(DESTDIR)$(BUSSERVICEDIR)"
	
	$(info installing systemd service file.)
	@$(INSTALL_DIR) "$(DESTDIR)$(SYSTEMDDIR)"
	@$(INSTALL_DATA) $(EXTRADIR)/$(SYSTEMDSERVICE) "$(DESTDIR)$(SYSTEMDDIR)"
	
	$(info installing polkit policy file.)
	@$(INSTALL_DIR) "$(DESTDIR)$(POLKITPOLICYDIR)"
	@$(INSTALL_DATA) $(EXTRADIR)/$(POLKITPOLICYNAME) "$(DESTDIR)$(POLKITPOLICYDIR)"

uninstall:
	$(info uninstalling bin.)
	@$(RM) "$(DESTDIR)$(BINDIR)/$(BINNAME)"

	$(info uninstalling dbus conf file.)
	@$(RM) "$(DESTDIR)$(BUSCONFDIR)/$(BUSCONFNAME)"

	$(info uninstalling dbus service file.)
	@$(RM) "$(DESTDIR)$(BUSSERVICEDIR)/$(BUSSERVICENAME)"
	
	$(info uninstalling systemd service file.)
	@$(RM) "$(DESTDIR)$(SYSTEMDDIR)/$(SYSTEMDSERVICE)"
	
	$(info uninstalling polkit policy file.)
	@$(RM) "$(DESTDIR)$(POLKITPOLICYDIR)/$(POLKITPOLICYNAME)"
