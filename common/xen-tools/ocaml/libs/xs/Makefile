TOPLEVEL=$(CURDIR)/../..
XEN_ROOT=$(TOPLEVEL)/../..
include $(TOPLEVEL)/common.make

OCAMLINCLUDE += -I ../xb/
OCAMLOPTFLAGS += -for-pack Xenstore

.NOTPARALLEL:
# Ocaml is such a PITA!

PREINTF = xsraw.cmi xst.cmi
PREOBJS = queueop xsraw xst
PRELIBS = $(foreach obj, $(PREOBJS),$(obj).cmo) $(foreach obj,$(PREOJBS),$(obj).cmx)
OBJS = queueop xsraw xst xs
INTF = xsraw.cmi xst.cmi xs.cmi
LIBS = xenstore.cma xenstore.cmxa

all: $(PREINTF) $(PRELIBS) $(INTF) $(LIBS) $(PROGRAMS)

bins: $(PROGRAMS)

libs: $(LIBS)

xenstore_OBJS = xenstore
OCAML_NOC_LIBRARY = xenstore

xenstore.cmx : $(foreach obj, $(OBJS), $(obj).cmx)
	$(E) " CMX      $@"
	$(Q)$(OCAMLOPT) -pack -o $@ $^

xenstore.cmo : $(foreach obj, $(OBJS), $(obj).cmo)
	$(E) " CMO      $@"
	$(Q)$(OCAMLC) -pack -o $@ $^


.PHONY: install
install: $(LIBS) META
	mkdir -p $(OCAMLDESTDIR)
	ocamlfind remove -destdir $(OCAMLDESTDIR) xenstore
	ocamlfind install -destdir $(OCAMLDESTDIR) -ldconf ignore xenstore META $(LIBS) xenstore.cmo xenstore.cmi xenstore.cmx *.a 

.PHONY: uninstall
uninstall:
	ocamlfind remove -destdir $(OCAMLDESTDIR) xenstore

include $(TOPLEVEL)/Makefile.rules

