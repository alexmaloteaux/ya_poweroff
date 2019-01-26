all : payload convert nro poweroff

clean : nro payload
	rm nro/source/payload.h
	rm poweroff.nro

payload:
	$(MAKE) -C $@ $(MAKECMDGOALS)
	
nro:
	$(MAKE) -C $@ $(MAKECMDGOALS)
	
convert:
	python2 tools/binConverter.py

poweroff:
	cp nro/$@.nro .


.PHONY:  nro payload clean convert poweroff
