# 3DS Link

## v1.1 — PWA UI + Color Fix

### Colorimétrie
Le RGB565 de CAMU est décodé avec le rouge dans les bits hauts et le bleu dans
les bits bas pour les images BMP/Safari. Le viseur natif 3DS reste inchangé.

### Interface iPhone
- design de type application/PWA ;
- barre supérieure translucide ;
- dock inférieur fixe ;
- cartes arrondies et surfaces vitrées ;
- meta tags iOS standalone ;
- manifest Web App et icône SVG servis directement par la 3DS ;
- Camera Link intégré au même langage visuel.

### Interface 3DS
- en-têtes sombres plus proches d'une application système ;
- accent bleu commun avec l'iPhone ;
- Camera Link et écran d'activité harmonisés ;
- aucun changement dans le moteur caméra natif v0.8.

Note : la page est servie sur le réseau local en HTTP. Le design et le mode
"web app" iOS sont présents, mais certaines fonctions PWA modernes comme les
Service Workers exigent normalement HTTPS.
