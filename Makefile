dirroof: dirroof.c recursive.c recursive.h VERSION Makefile
	@version=$$(cat VERSION) && \
	    case "$$version" in \
	        [!0-9]*|''|*[!0-9A-Za-z.+~]*) echo "Invalid dirroof version: $$version" >&2; exit 1;; \
	    esac && \
	    $(CC) $(CPPFLAGS) $(CFLAGS) "-DVERSION=\"$$version\"" $(LDFLAGS) -o $@ dirroof.c recursive.c $(LDLIBS)
