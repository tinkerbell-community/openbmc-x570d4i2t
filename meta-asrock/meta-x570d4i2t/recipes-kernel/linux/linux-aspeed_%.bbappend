FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"
SRC_URI:append = " \
    file://x570d4i2t.cfg \
    file://0001-aspeed-add-x570d4i2t-dts.patch \
"
