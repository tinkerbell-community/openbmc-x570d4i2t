FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://channel_config.json \
    file://master_write_read_white_list.json \
    "

do_install:append() {
    install -m 0644 ${UNPACKDIR}/channel_config.json \
        ${D}${datadir}/ipmi-providers/channel_config.json
    install -m 0644 ${UNPACKDIR}/master_write_read_white_list.json \
        ${D}${datadir}/ipmi-providers/master_write_read_white_list.json
}

FILES:${PN}:append = " \
    ${datadir}/ipmi-providers/channel_config.json \
    ${datadir}/ipmi-providers/master_write_read_white_list.json \
    "
