FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

SRC_URI:append = " \
    file://0001-Add-color-mode-type-media-query.patch \
    file://0002-Use-css-variables-in-custom-scss.patch \
    "
