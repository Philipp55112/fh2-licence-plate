# Lichtfarbe je Auto (Kennzeichenlicht + Lampenglas). v64: kraeftiger, damit Halogen sichtbar warm ist
import json
LT=json.load(open('/home/claude/work/v58/lighttype.json'))
RGB={'HALOGEN':(1.0,0.70,0.40),'XENON':(0.80,0.90,1.0),'LED':(0.88,0.94,1.0)}
def car_rgb(car): return RGB[LT.get(car,'HALOGEN').rstrip('?')]
