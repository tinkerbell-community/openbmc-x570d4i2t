FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

SRC_URI:append = " \
    file://0001-Add-color-mode-type-media-query.patch \
    "

# NOTE: 0002-Use-css-variables-in-custom-scss.patch is temporarily disabled — it
# no longer applies against the current webui-vue revision (stale _card.scss hunk,
# malformed git blob indices) and blocks the image build. Re-add it here once
# rebased to restore the Bootstrap-5 css-variable theming.
