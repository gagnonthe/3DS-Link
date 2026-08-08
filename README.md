# 3DS Link

## v0.6 — Camera pipeline fix

Cette version corrige le pipeline caméra après les problèmes observés sur la vraie console.

### Ce qui a été corrigé

- l'aperçu 3DS n'envoie plus une image RGBA linéaire directement à `C3D_TexUpload` ;
- le buffer caméra RGB565 est maintenant converti vers la disposition **tuilée 8×8 Morton** attendue par le GPU PICA200 ;
- le viseur utilise directement une texture `GPU_RGB565`, ce qui évite une conversion couleur inutile pour l'écran 3DS ;
- la conversion RGB565 → BMP garde l'ordre de composantes utilisé par l'exemple caméra officiel devkitPro ;
- le mode caméra passe sur une fréquence adaptative `15_TO_5`, plus adaptée lorsque la lumière est faible ;
- mode photo, contraste et correction de lentille sont explicitement initialisés ;
- la caméra attend davantage de frames avant d'autoriser une photo afin de laisser l'exposition et la balance des blancs se stabiliser ;
- le flux iPhone et les BMP continuent d'utiliser le même frame source que le viseur.

### Pourquoi les bandes rouges apparaissaient

Citro3D/PICA200 ne lit pas une texture dynamique comme un tableau classique rangé ligne par ligne.
La v0.5 envoyait pourtant notre buffer RGBA8 linéaire directement à `C3D_TexUpload`.
Le GPU interprétait alors ces octets comme des tuiles 8×8, d'où les motifs verticaux répétitifs.

### Référence

La logique de capture et l'ordre RGB565 suivent l'exemple officiel
`devkitPro/3ds-examples/camera/video`.

### Test conseillé

1. Ouvre Camera Link.
2. Attends environ 1 à 2 secondes.
3. Vérifie le viseur de la 3DS.
4. Vérifie ensuite le flux sur l'iPhone.
5. Prends une photo d'un objet avec plusieurs couleurs faciles à reconnaître.
6. Compare viseur 3DS, flux Safari et BMP téléchargé.
