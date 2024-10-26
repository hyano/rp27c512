#!/bin/sh -x


PROJECT1=rp27c512-panel
PROJECT2=rp27c512
OUTPUT=elecrow

mkdir "$OUTPUT"

cp "$PROJECT1"-F_Cu.gtl "$OUTPUT"/"$PROJECT2".GTL
cp "$PROJECT1"-B_Cu.gbl "$OUTPUT"/"$PROJECT2".GBL
cp "$PROJECT1"-F_Mask.gts "$OUTPUT"/"$PROJECT2".GTS
cp "$PROJECT1"-B_Mask.gbs "$OUTPUT"/"$PROJECT2".GBS
cp "$PROJECT1"-F_Silkscreen.gto "$OUTPUT"/"$PROJECT2".GTO
cp "$PROJECT1"-B_Silkscreen.gbo "$OUTPUT"/"$PROJECT2".GBO
cp "$PROJECT1".drl "$OUTPUT"/"$PROJECT2".TXT
#cp "$PROJECT1"-NPTH.drl "$OUTPUT"/"$PROJECT2"-NPTH.TXT
cp "$PROJECT1"-Edge_Cuts.gm1 "$OUTPUT"/"$PROJECT2".GML

cd "$OUTPUT"
zip ../rp27c512_elecrow.zip *
cd ..

