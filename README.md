# 3DS Link

## v0.9.1 — Build fix

Correctif de compilation de la v0.9.

### Correction

- suppression de `SO_RCVTIMEO` et `SO_SNDTIMEO`, non disponibles dans l'environnement socket libctru utilisé par devkitARM ;
- conservation du moteur caméra v0.8/v0.9 inchangé ;
- conservation de l'actualisation immédiate du compteur de photos ;
- conservation de la synchronisation iPhone accélérée prévue par la v0.9.

Les avertissements concernant certaines fonctions non utilisées ne bloquent pas la compilation.
