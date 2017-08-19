FATE_FITS += fate-fitsdec-multi
fate-fitsdec-multi: CMD = framecrc -i $(TARGET_SAMPLES)/fits/fits-multi.fits -pix_fmt gbrap

fate-fitsdec%: PIXFMT = $(word 3, $(subst -, ,$(@)))
fate-fitsdec%: SRC = $(TARGET_SAMPLES)/fits/lena-$(PIXFMT).fits
fate-fitsdec%: CMD = framecrc -i $(SRC) -pix_fmt $(PIXFMT)

FATE_FITS_DEC_PIXFMT = gray gray16 gbrp gbrp16 gbrap16
FATE_FITS += $(FATE_FITS_DEC_PIXFMT:%=fate-fitsdec-%)

fate-fitsenc%: fate-fitsdec-multi
fate-fitsenc%: PIXFMT = $(word 3, $(subst -, ,$(@)))
fate-fitsenc%: SRC = $(TARGET_SAMPLES)/fits/fits-multi.fits
fate-fitsenc%: CMD = framecrc -i $(SRC) -c:v fits -pix_fmt $(PIXFMT)

FATE_FITS_ENC_PIXFMT = gray gray16be gbrp gbrap gbrp16be gbrap16be
FATE_FITS_ENC-$(call ENCDEC, FITS, FITS) = $(FATE_FITS_ENC_PIXFMT:%=fate-fitsenc-%)

FATE_FITS += $(FATE_FITS_ENC-yes)
fate-fitsenc: $(FATE_FITS_ENC-yes)

FATE_FITS-$(call DEMDEC, FITS, FITS) += $(FATE_FITS)

FATE_SAMPLES_FFMPEG += $(FATE_FITS-yes)
fate-fits: $(FATE_FITS-yes)
