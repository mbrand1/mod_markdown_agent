# Makefile for mod_markdown_agent
#
# Assumes Micah's custom-compiled Apache layout:
#   /usr/local/apache/bin/apxs
#   /usr/local/apache/conf/
#   /usr/local/apache/modules/
#
# libxml2 is expected at the standard system paths.
# If installed elsewhere: make LIBXML2_INC=/path/to/include/libxml2

APXS        = /usr/local/apache/bin/apxs
LIBXML2_INC = /usr/include/libxml2
LIBXML2_LIB = -lxml2

SRCS        = mod_markdown_agent.c html2md.c

# apxs handles -I$(AP_INCLUDEDIR) and most flags automatically.
# We pass our extra includes and libs via -Wc and -Wl.

.PHONY: all install clean reload

all:
	$(APXS) -c \
		-Wc,-Wall \
		-Wc,-I$(LIBXML2_INC) \
		-Wl,$(LIBXML2_LIB) \
		$(SRCS)

install: all
	$(APXS) -i -a -n markdown_agent mod_markdown_agent.la
	@echo ""
	@echo "Module installed. Add to httpd.conf if not already present:"
	@echo "  LoadModule markdown_agent_module modules/mod_markdown_agent.so"
	@echo ""
	@echo "Then: apachectl configtest && apachectl graceful"

clean:
	rm -f *.o *.lo *.la *.so *.slo
	rm -rf .libs

reload:
	/usr/local/apache/bin/apachectl graceful

# -----------------------------------------------------------------------
# Quick smoke test: curl with Accept: text/markdown against localhost
# Usage: make test URL=http://localhost/some-page.html
# -----------------------------------------------------------------------
test:
	@echo "--- Response headers ---"
	curl -sI -H "Accept: text/markdown" "$(URL)"
	@echo ""
	@echo "--- Response body (first 40 lines) ---"
	curl -s  -H "Accept: text/markdown" "$(URL)" | head -40
