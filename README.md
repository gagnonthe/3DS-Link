# 3DS Link

## v0.7 — Stable 3DS Camera Preview

Cette version se concentre uniquement sur la caméra de la 3DS.

### Changement d'architecture

La v0.5/v0.6 faisait passer le flux caméra dans une texture Citro2D/Citro3D.
Sur la vraie console cela provoquait encore des saccades, une mauvaise position de
l'image, des couleurs instables et du clignotement.

La v0.7 supprime complètement cette étape pour le viseur.

Le buffer `OUTPUT_RGB_565` de CAMU est maintenant affiché directement dans le
framebuffer RGB8 de l'écran supérieur, avec la même organisation mémoire que
l'exemple officiel `devkitPro/3ds-examples/camera/video`.

### Autres corrections

- attente bloquante de la vraie frame suivante au lieu d'un simple polling à 0 ns ;
- une nouvelle réception CAMU est armée à chaque frame ;
- resynchronisation de la capture si une réception échoue ;
- aucune opération réseau pendant le mode caméra ;
- aucun rendu Citro2D/Citro3D sur l'écran supérieur pendant le direct ;
- les deux buffers de l'écran inférieur contiennent la même interface afin
  d'éviter son clignotement pendant les swaps ;
- le flux direct Safari est volontairement désactivé dans cette version de test.

### Test attendu

1. Ouvre Camera Link avec `Y`.
2. Bouge lentement la 3DS.
3. L'image doit suivre continuellement, sans rester bloquée sur la première frame.
4. Elle doit occuper toujours exactement les 400×240 pixels de l'écran supérieur.
5. Il ne doit plus y avoir de clignotement causé par Citro2D.
6. Après 1 à 2 secondes, teste une photo avec `A`.

Une fois ce viseur confirmé stable sur une vraie console, le streaming iPhone
sera réintroduit séparément afin de ne pas dégrader la boucle caméra.
