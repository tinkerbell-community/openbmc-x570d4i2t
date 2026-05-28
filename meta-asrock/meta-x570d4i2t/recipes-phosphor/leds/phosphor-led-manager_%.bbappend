FILESEXTRAPATHS:prepend:x570d4i2t := "${THISDIR}/${PN}:"

SRC_URI:append:x570d4i2t = " file://led-group-config.json"

do_install:append:x570d4i2t() {
        install -m 0644 ${UNPACKDIR}/led-group-config.json ${D}${datadir}/phosphor-led-manager/
}
