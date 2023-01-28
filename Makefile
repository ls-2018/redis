# Top level makefile, the real shit is at src/Makefile

default: all

.DEFAULT:
	cd src && sh './mkreleasehdr.sh' && cd ..
	rm ./dump.rdb || rm ./src/dump.rdb || rm ./src/release.h || echo start
	cd src && $(MAKE) $@

install:
	cd src && $(MAKE) $@

.PHONY: install

lint:
	find . -name '*.c' | grep -v deps | grep -v tests | xargs clang-format -style=file -i
	find . -name '*.h' | grep -v deps | grep -v tests | xargs clang-format -style=file -i

sync:
	env https_proxy=https://127.0.0.1:7890 && git push --force