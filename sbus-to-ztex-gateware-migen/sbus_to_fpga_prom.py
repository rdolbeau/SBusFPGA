import os
import json
import inspect
from shutil import which
from sysconfig import get_platform

from migen import *

import bw2_fb
import cg3_fb
import cg6_fb
from VintageBusFPGA_Common.goblin_fb import *


def get_header_map_stuff(gname, name, size, type="csr", reg=True):
    shorten = { "csr" : "c",
                "region" : "r" }
    
    r = ""
    if (reg):
        r += f"my-address sf{shorten[type]}a_{name} + my-space h# {size:x} reg\n"
    r += "h# 7f encode-int \" slave-burst-sizes\" property\n" # fixme: burst-sizes
    r += "h# 7f encode-int \" burst-sizes\" property\n" # fixme: burst-sizes
    r += "headers\n"
    r += f"-1 instance value {name}-virt\nmy-address constant my-sbus-address\nmy-space constant my-sbus-space\n"
    r += ": map-in ( adr space size -- virt ) \" map-in\" $call-parent ;\n: map-out ( virt size -- ) \" map-out\" $call-parent ;\n";
    r += f": map-in-{gname} ( -- ) my-sbus-address sf{shorten[type]}a_{name} + my-sbus-space h# {size:x} map-in to {name}-virt ;\n"
    r += f": map-out-{gname} ( -- ) {name}-virt h# {size:x} map-out ;\n"
    return r

def get_header_map2_stuff(gname, name1, name2, size1, size2, type1="csr", type2="csr"):
    shorten = { "csr" : "c",
                "region" : "r" }
        
    r  = f"my-address sf{shorten[type1]}a_{name1} + my-space encode-phys         h# {size1:x} encode-int encode+\n"
    r += f"my-address sf{shorten[type2]}a_{name2} + my-space encode-phys encode+ h# {size2:x} encode-int encode+\n"
    r += "\" reg\" property\n"
    r += "h# 7f encode-int \" slave-burst-sizes\" property\n" # fixme: burst-sizes
    r += "h# 7f encode-int \" burst-sizes\" property\n" # fixme: burst-sizes
    r += "headers\n"
    r += f"-1 instance value {name1}-virt\n"
    r += f"-1 instance value {name2}-virt\n"
    r += "my-address constant my-sbus-address\nmy-space constant my-sbus-space\n"
    r += ": map-in ( adr space size -- virt ) \" map-in\" $call-parent ;\n: map-out ( virt size -- ) \" map-out\" $call-parent ;\n";
    r += f": map-in-{gname} ( -- )\n"
    r += f"my-sbus-address sf{shorten[type1]}a_{name1} + my-sbus-space h# {size1:x} map-in to {name1}-virt\n"
    r += f"my-sbus-address sf{shorten[type2]}a_{name2} + my-sbus-space h# {size2:x} map-in to {name2}-virt\n"
    r += ";\n"
    r += f": map-out-{gname} ( -- )\n"
    r += f"{name1}-virt h# {size1:x} map-out\n"
    r += f"{name2}-virt h# {size2:x} map-out\n"
    r += ";\n"
    return r

def get_header_map3_stuff(gname, name1, name2, name3, size1, size2, size3, type1="csr", type2="csr", type3="csr", doreg=True):
    shorten = { "csr" : "c",
                "region" : "r" }
        
    if (doreg):
        r  = f"my-address sf{shorten[type1]}a_{name1} + my-space encode-phys         h# {size1:x} encode-int encode+\n"
        r += f"my-address sf{shorten[type2]}a_{name2} + my-space encode-phys encode+ h# {size2:x} encode-int encode+\n"
        r += f"my-address sf{shorten[type3]}a_{name3} + my-space encode-phys encode+ h# {size3:x} encode-int encode+\n"
        r += "\" reg\" property\n"
        r += "h# 7f encode-int \" slave-burst-sizes\" property\n" # fixme: burst-sizes
        r += "h# 7f encode-int \" burst-sizes\" property\n" # fixme: burst-sizes
    else:
        r = ""
    r += "headers\n"
    r += f"-1 instance value {name1}-virt\n"
    r += f"-1 instance value {name2}-virt\n"
    r += f"-1 instance value {name3}-virt\n"
    r += "my-address constant my-sbus-address\nmy-space constant my-sbus-space\n"
    r += ": map-in ( adr space size -- virt ) \" map-in\" $call-parent ;\n: map-out ( virt size -- ) \" map-out\" $call-parent ;\n";
    r += f": map-in-{gname} ( -- )\n"
    r += f"my-sbus-address sf{shorten[type1]}a_{name1} + my-sbus-space h# {size1:x} map-in to {name1}-virt\n"
    r += f"my-sbus-address sf{shorten[type2]}a_{name2} + my-sbus-space h# {size2:x} map-in to {name2}-virt\n"
    r += f"my-sbus-address sf{shorten[type3]}a_{name3} + my-sbus-space h# {size3:x} map-in to {name3}-virt\n"
    r += ";\n"
    r += f": map-out-{gname} ( -- )\n"
    r += f"{name1}-virt h# {size1:x} map-out\n"
    r += f"{name2}-virt h# {size2:x} map-out\n"
    r += f"{name3}-virt h# {size3:x} map-out\n"
    r += ";\n"
    return r

def get_header_mapx_stuff(gname, names, sizes, types, doreg=True):
    shorten = { "csr" : "c",
                "region" : "r" }

    if (doreg):
        r  = f"my-address sf{shorten[types[0]]}a_{names[0]} + my-space encode-phys             h# {sizes[0]:x} encode-int encode+\n"
        for i in range(1, len(names)):
            r += f"my-address sf{shorten[types[i]]}a_{names[i]} + my-space encode-phys encode+ h# {sizes[i]:x} encode-int encode+\n"
        r += "\" reg\" property\n"
        r += "h# 7f encode-int \" slave-burst-sizes\" property\n" # fixme: burst-sizes
        r += "h# 7f encode-int \" burst-sizes\" property\n" # fixme: burst-sizes
    else:
        r = ""
    r += "headers\n"
    for i in range(0, len(names)):
        r += f"-1 instance value {names[i]}-virt\n"
    r += "my-address constant my-sbus-address\nmy-space constant my-sbus-space\n"
    r += ": map-in ( adr space size -- virt ) \" map-in\" $call-parent ;\n: map-out ( virt size -- ) \" map-out\" $call-parent ;\n";
    r += f": map-in-{gname} ( -- )\n"
    for i in range(0, len(names)):
        r += f"my-sbus-address sf{shorten[types[i]]}a_{names[i]} + my-sbus-space h# {sizes[i]:x} map-in to {names[i]}-virt\n"
    r += ";\n"
    r += f": map-out-{gname} ( -- )\n"
    for i in range(0, len(names)):
        r += f"{names[i]}-virt h# {sizes[i]:x} map-out\n"
    r += ";\n"
    return r

def get_prom(soc,
             version,
             sys_clk_freq,
             stat=False,
             trng=False,
             usb=False,
             sdram=True,
             engine=False,
             i2c=False,
             bw2=False,
             cg3=False,
             cg6=False,
             goblin=False,
             cg3_res=None,
             sdcard=False,
             jareth=False):

    framebuffer = (bw2 or cg3 or cg6 or goblin)
    
    r = "fcode-version2\nfload prom_csr_{}.fth\n".format(version.replace(".", "_"))

    if (version == "V1.0"):
        r += "\" RDOL,led\" device-name\n"
        r += get_header_map_stuff("leds", "leds", 4)
        r += ": setled! ( pattern -- )\nmap-in-leds\nleds-virt l! ( pattern virt -- )\nmap-out-leds\n;\n"
        r += "finish-device\nnew-device\n" # this assumes at least one device is active, which is true for sdram (we always have at least initialization)

    if (stat):
        r += "\" RDOL,sbusstat\" device-name\n"
        r += get_header_map_stuff("sbus_bus_stat", "sbus_bus_stat", 256)
        if (trng or usb or (sdram or not sdram) or engine or i2c or framebuffer or sdcard):
            r += "finish-device\nnew-device\n"

    if (trng):
        r += "\" RDOL,neorv32trng\" device-name\n"
        r += get_header_map_stuff("trng", "trng", 8)
        r += ": disabletrng! ( -- )\n"
        r += "  map-in-trng\n"
        r += "  1 trng-virt l! ( pattern virt -- )\n"
        r += "  map-out-trng\n"
        r += ";\n"
        r += "disabletrng!\n"
        if (usb or (sdram or not sdram) or engine or i2c or framebuffer or sdcard):
            r += "finish-device\nnew-device\n"

    if (usb):
        r += "\" generic-ohci\" device-name\n"
        r += "sbusfpga_irq_usb_host encode-int \" interrupts\" property\n"
        r += get_header_map_stuff("usb_host_ctrl", "usb_host_ctrl", 4096, type="region")
        r += ": my-reset! ( -- )\n"
        r += " map-in-usb_host_ctrl\n"
        r += " 00000001 usb_host_ctrl-virt h#  4 + l! ( -- ) ( reset the HC )\n"
        r += " 00000000 usb_host_ctrl-virt h# 18 + l! ( -- ) ( reset HCCA & friends )\n"
        r += " 00000000 usb_host_ctrl-virt h# 1c + l! ( -- )\n"
        r += " 00000000 usb_host_ctrl-virt h# 20 + l! ( -- )\n"
        r += " 00000000 usb_host_ctrl-virt h# 24 + l! ( -- )\n"
        r += " 00000000 usb_host_ctrl-virt h# 28 + l! ( -- )\n"
        r += " 00000000 usb_host_ctrl-virt h# 2c + l! ( -- )\n"
        r += " 00000000 usb_host_ctrl-virt h# 30 + l! ( -- )\n"
        r += " map-out-usb_host_ctrl\n"
        r += ";\n"
        r += "my-reset!\n"
        if ((sdram or not sdram) or engine or i2c or framebuffer or sdcard):
            r += "finish-device\nnew-device\n"
        
    if (sdram):
        r += "\" RDOL,sdram\" device-name\n"
        r += get_header_mapx_stuff("mregs", [ "ddrphy", "sdram", "exchange_with_mem" ], [ 4096, 4096, 4096 ], [ "csr", "csr", "csr" ])
        r += "sbusfpga_irq_sdram encode-int \" interrupts\" property\n"
    else:
        r += "\" RDOL,hidden_sdram\" device-name\n"
        r += get_header_mapx_stuff("mregs", [ "ddrphy", "sdram" ], [ 4096, 4096 ], [ "csr", "csr" ])
        
    if (sys_clk_freq == 100e6):
        r += "h# 19 constant m0_delay\n"
        r += "h# 19 constant m1_delay\n"
        r += "h# 1 constant m0_bitslip\n"
        r += "h# 1 constant m1_bitslip\n"
    elif (sys_clk_freq == 90e6):
        r += "h# 1c constant m0_delay\n"
        r += "h# 1c constant m1_delay\n"
        r += "h# 1 constant m0_bitslip\n"
        r += "h# 1 constant m1_bitslip\n"
    else:
        print("UNCALIBRATED FREQUENCY for SDRAM!")
        assert(False)
        
    r += "fload sdram_init.fth\ninit!\n"
    if (engine or i2c or framebuffer or sdcard):
        r += "finish-device\nnew-device\n"
    
    if (engine):
        r += "\" betrustedc25519e\" device-name\n"
        r += ": sfra_curve25519engine-microcode sfra_curve25519engine ;\n"
        r += ": sfra_curve25519engine-regfile sfra_curve25519engine h# 10000 + ;\n"
        r += get_header_mapx_stuff("curve25519engine", [ "curve25519engine", "curve25519engine-microcode", "curve25519engine-regfile" ], [ 4096, 4096, 65536 ] , ["csr", "region", "region" ] )
        if (i2c or framebuffer or sdcard):
            r += "finish-device\nnew-device\n"
        
    if (i2c):
        r += "\" oc,i2c\" device-name\n"
        r += get_header_map_stuff("i2c", "i2c", 64)
        r += (f"h# {sys_clk_freq:x} encode-int \" clock-speed\" property\n").replace("0x", "")
        bus_speed = 400000
        r += (f"h# {bus_speed:x} encode-int \" bus-speed\" property\n").replace("0x", "")
        # assume just a lm75-compatible device at 0x48 (i.e. the custom Pmod)
        r += "  new-device\n"
        r += "  \" AT30TS74-UFM10\" encode-string \" name\" property\n"
        r += "  \" lm75\" encode-string \" compatible\" property\n"
        r += "  h# 48 encode-int \" addr\" property\n"
        r += "  finish-device\n"
        if (framebuffer or sdcard):
            r += "finish-device\nnew-device\n"
        
    if (framebuffer):
        hres = int(cg3_res.split("@")[0].split("x")[0])
        vres = int(cg3_res.split("@")[0].split("x")[1])
        hres_h=(f"{hres:x}").replace("0x", "")
        vres_h=(f"{vres:x}").replace("0x", "")

        if (goblin):
            if (jareth):
                r += get_header_mapx_stuff("jareth", [ "goblin_accel" ], [ 4096 ], [ "region" ], doreg=False )
                r += "h# 1 constant goblin-has-jareth\n"
                r += "fload goblin_jareth_define.fth\n"
                r += "fload goblin_jareth_init.fth\n"
            else:
                # use some placeholder so the 'reg' property works
                r += "h# -1 constant sfra_jareth-regs\n"
                r += "h# 0 constant goblin-has-jareth\n"
        
        if (bw2):
            cg3_file = open("bw2.fth")
        elif (cg3):
            cg3_file = open("cg3.fth")
        elif (cg6):
            cg3_file = open("cg6.fth")
        elif (goblin):
            cg3_file = open("goblin.fth")
        cg3_lines = cg3_file.readlines()
        if (bw2):
            buf_size=bw2_fb.bw2_rounded_size(hres, vres)
        elif (cg3 or cg6):
            buf_size=cg3_fb.cg3_rounded_size(hres, vres)
        elif (goblin):
            buf_size=goblin_rounded_size(hres, vres, "SBus")
        for line in cg3_lines:
            r += line.replace("SBUSFPGA_CG3_WIDTH", hres_h).replace("SBUSFPGA_CG3_HEIGHT", vres_h).replace("SBUSFPGA_CG3_BUFSIZE", f"{buf_size:x}")
        #r += "\" LITEX,fb\" device-name\n"
        #r += get_header_map2_stuff("cg3extraregs", "vid_fb", "vid_fb_vtg", 4096, 4096)
        #r += "fload fb_init.fth\nfb_init!\n"
        r += "\n"
        if (bw2):
            r += get_header_map_stuff("bw2extraregs", "bw2", 4096, reg=False)
            r += "fload bw2_init.fth\nbw2_init!\n"
        elif (cg3):
            r += get_header_map_stuff("cg3extraregs", "cg3", 4096, reg=False)
            r += "fload cg3_init.fth\ncg3_init!\n"
        elif (cg6):
            r += get_header_map_stuff("cg6extraregs", "cg6", 4096, reg=False)
            r += "fload cg6_init.fth\ncg6_init!\n"
        elif (goblin):
            r += "\n"
        if (sdcard):
            r += "finish-device\nnew-device\n"
        
    if (sdcard):
        r += "\" LITEX,sdcard\" device-name\n"
        r += get_header_mapx_stuff("sdcard", ["sdcore", "sdirq", "sdphy", "sdblock2mem", "sdmem2block" ], [ 4096, 4096, 4096, 4096, 4096 ], [ "csr", "csr", "csr", "csr", "csr" ] )
        r += ": sdcard-init!\n"
        r += "  map-in-sdcard\n"
        r += "  0 sdirq-virt h# 8 + l! ( disable irqs )\n"
        r += "  0 sdblock2mem-virt h# c + l! ( disable dma )\n"
        r += "  0 sdmem2block-virt h# c + l! ( disable dma )\n"
        r += "  map-out-sdcard\n"
        r += ";\n"
        r += "sdcard-init!\n"
        r += "fload sdcard.fth\n"
        r += "fload sdcard_access.fth\n"

    r += "end0\n"

    return r
