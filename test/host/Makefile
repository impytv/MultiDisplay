# Host tests (see main.c). Needs gcc and an ESP-IDF checkout for cJSON:
#   make            build and run
#   make IDF_PATH=/opt/esp/idf
IDF_PATH ?= $(HOME)/esp/esp-idf
ROOT     := ../..
CJSON    := $(IDF_PATH)/components/json/cJSON

CFLAGS  += -std=gnu17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Werror \
           -fsanitize=address,undefined -fno-omit-frame-pointer \
           -include stubs/sdkconfig.h -Istubs -I. -I$(ROOT)/components -I$(ROOT)/main -I$(CJSON)
LDFLAGS += -fsanitize=address,undefined
LDLIBS  += -lm

SRCS := main.c stubs/stubs.c test_version.c test_form.c test_http.c test_sun.c test_entur.c \
        test_adsb.c test_routes.c test_config.c test_air.c test_yr.c test_tide.c test_ical.c test_sat.c test_aurora.c \
        $(ROOT)/components/app_config.c $(ROOT)/components/app_config_json.c $(CJSON)/cJSON.c

run: host_tests
	./host_tests

host_tests: $(SRCS) $(wildcard *.h stubs/*.h $(ROOT)/components/*.[ch] $(ROOT)/main/sun.c)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f host_tests

.PHONY: run clean
