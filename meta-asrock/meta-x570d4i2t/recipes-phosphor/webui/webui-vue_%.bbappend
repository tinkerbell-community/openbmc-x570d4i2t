FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

# Build the web UI from the pi-bmc fork instead of openbmc/webui-vue upstream.
# Override both the git URL and SRCREV — the upstream recipe pins its own
# SRCREV, so it must be overridden in lockstep with the URL.
SRC_URI = "git://github.com/pi-bmc/webui-vue.git;branch=master;protocol=https"
SRCREV = "9777cd0a3832390765d0ae4571c2783ceb3165e6"
