.PHONY: build test release clean
build:
	bash scripts/build.sh
test:
	$(MAKE) -C tests test
release: test build
	python3 scripts/package.py
	python3 tests/package_test.py
clean:
	rm -rf build dist
	$(MAKE) -C tests clean
