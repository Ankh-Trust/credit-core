#!/bin/bash
# create multiresolution windows icon
#mainnet
ICON_SRC=../../src/qt/res/icons/credit.png
ICON_DST=../../src/qt/res/icons/credit.ico
convert ${ICON_SRC} -resize 16x16 credit-16.png
convert ${ICON_SRC} -resize 32x32 credit-32.png
convert ${ICON_SRC} -resize 48x48 credit-48.png
convert credit-16.png credit-32.png credit-48.png ${ICON_DST}
#testnet
ICON_SRC=../../src/qt/res/icons/credit_testnet.png
ICON_DST=../../src/qt/res/icons/credit_testnet.ico
convert ${ICON_SRC} -resize 16x16 credit-16.png
convert ${ICON_SRC} -resize 32x32 credit-32.png
convert ${ICON_SRC} -resize 48x48 credit-48.png
convert credit-16.png credit-32.png credit-48.png ${ICON_DST}
rm credit-16.png credit-32.png credit-48.png
