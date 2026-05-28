FILESEXTRAPATHS:prepend:x570d4i2t := "${THISDIR}/${PN}:"

SRC_URI:append:x570d4i2t = " file://x570d4i2t.json"

do_install:append:x570d4i2t() {
        install -m 0444 ${UNPACKDIR}/x570d4i2t.json \
            ${D}${datadir}/entity-manager/configurations/
}
