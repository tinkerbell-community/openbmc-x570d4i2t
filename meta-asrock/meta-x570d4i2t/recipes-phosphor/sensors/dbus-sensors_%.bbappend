# intelcpusensor is Intel PECI-only; this is an AMD Ryzen board, so it never
# finds a CPU and just logs "error getting SpecialMode status" at boot. Drop it.
PACKAGECONFIG = " \
        adcsensor \
        fansensor \
        hwmontempsensor \
        ipmbsensor \
        nvmesensor \
        psusensor \
        "
