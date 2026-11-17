# Environnement de développement et déploiement Raspberry Pi

## État de préparation — 17 novembre 2026

Objectif : compiler le fork `2.7-evo` sans modification du code fonctionnel,
puis valider Air et Ground avant l'intégration OpenDroneID.

- Pi : Zero 2 W, Raspbian Bullseye armhf, OpenHD 2.6.4.
- Source de référence : `c75c37e6bcd014a990b81ae9b2556eb6afefd924` ; les commits
  suivants au moment de cet audit ne changent que la documentation.
- Windows : Git disponible ; Docker, CMake et Ninja non détectés dans le PATH.
- L'invocation de WSL a déclenché l'installation de WSL 3.0.5 et l'activation
  de VirtualMachinePlatform. Windows exige un redémarrage pour les activer.
- Aucune distribution WSL installée à ce stade.
- Aucun build ni déploiement 2.7 réalisé ; aucun service du Pi modifié.

**Blocage actuel : redémarrage Windows puis installation/configuration d'une
distribution Linux.** Aucun redémarrage automatique ne doit être lancé.

## Chaîne de build de référence du dépôt

Le [workflow Raspberry Pi](../.github/workflows/build_package_rpi.yml) décrit
une compilation dans un Raspberry Pi OS émulé, base `raspios_lite:2023-05-03`,
avec 4 Go supplémentaires, sous runner Ubuntu.

Il appelle [install_build_dep.sh](../install_build_dep.sh) pour la plateforme
`rpi`, puis [package.sh](../package.sh) avec les arguments `standard armhf
raspbian bullseye`.

Le script d'installation ajoute le dépôt Cloudsmith OpenHD release et installe
notamment libcamera-openhd, les bibliothèques GStreamer, Poco, libpcap, libsodium,
libnl, SDL2 et les outils de compilation. Il peut purger des paquets caméra et
forcer des écrasements : **à exécuter uniquement dans l'environnement de build
jetable, pas directement sur les Pi opérationnels ni sur le Linux hôte**.

Le packaging configure CMake depuis la racine du dépôt, compile avec tous les
cœurs et utilise fpm. Il écrit dans `/out` ; créer ce répertoire dans le build
isolé. Le script supprime un autre répertoire temporaire que son répertoire de
staging réel : utiliser un environnement neuf et un staging vide pour chaque
build, sans modifier le script upstream à cette étape.

Les workflows historiques publient aussi sur Cloudsmith avec un secret de
mainteneur : cette publication n'est pas nécessaire pour nos paquets locaux et
ne doit pas être tentée. Ne pas considérer les workflows comme validés simplement
parce qu'ils sont présents dans le dépôt.

## Plan de mise en place après redémarrage

1. Vérifier WSL puis installer une distribution Ubuntu adaptée aux outils de build.
2. Construire dans le système de fichiers Linux, hors du dossier OneDrive monté,
   pour conserver permissions et liens symboliques et éviter les lenteurs.
3. Récupérer le fork et les sous-modules à une révision figée.
4. Préparer un environnement Raspberry Pi OS Bullseye **armhf** isolé, avec QEMU
   ou un mécanisme équivalent à celui du workflow officiel ; Ubuntu x86 seul
   ne produit pas un binaire utilisable sur les Pi.
5. Vérifier la disponibilité des paquets Cloudsmith et noter leurs versions.
6. Installer les dépendances dans cet environnement, configurer Release et
   compiler le cœur sans changer ses sources ; limiter le parallélisme selon
   la mémoire de l'environnement.
7. Produire le paquet et relever SHA-256, architecture, révision, dépendances et
   versions des bibliothèques. Conserver les journaux de build.

Ne pas télécharger ni exécuter aveuglément une chaîne de scripts distante :
examiner la configuration des dépôts et les opérations système prévues.

## Compatibilité à résoudre avant installation

La branche récente reçoit configuration et inventaire Wi-Fi depuis SysUtils.
Un build réussi n'assure pas que les services installés dans l'image 2.6.4
fournissent les API attendues.

Avant déploiement : inventorier les services SysUtils du Pi, comparer les
protocoles de socket de la branche et identifier les versions compagnon
nécessaires. Vérifier également QOpenHD, libcamera et les plugins GStreamer.
Ne pas installer le paquet 2.7 si ces préconditions ne sont pas satisfaites.

Préserver noyau et pilotes `rtl88x2eu_ohd` tant qu'aucune incompatibilité démontrée
n'impose une migration d'image. Aucune mise à jour générale de l'OS.

## Déploiement séquentiel et récupération

L'utilisateur autorise les essais 2.7 sur les deux Pi et dispose d'ImageWriter
pour restaurer une image stable. Cela ne remplace pas la sauvegarde des réglages
et ne garantit pas la récupération à distance.

1. Exporter sur le PC les paramètres, unités système pertinentes et la version
   actuelle ; conserver le binaire 2.6.4 et, si disponible, son paquet.
2. Transférer les paquets et dépendances par SSH/SFTP ; vérifier les empreintes.
3. Simuler la résolution des dépendances et vérifier l'espace libre ; pas de
   contournement forcé des dépendances ni d'installation partielle.
4. Examiner les hooks [before-install.sh](../before-install.sh) et
   [after-install.sh](../after-install.sh) avant leur exécution sur le Pi.
5. Préparer un retour arrière local autonome avant l'arrêt d'OpenHD : son arrêt
   peut supprimer le hotspot et couper SSH. Un simple `nohup` n'est pas une
   stratégie de rollback suffisante.
6. Déployer l'Air accessible, relever les preuves de fonctionnement, puis demander
   à l'utilisateur de connecter Windows au hotspot Ground pour répéter l'opération.
7. Valider la paire seulement quand les deux côtés utilisent la combinaison
   logicielle choisie ; ne pas conclure sur la vidéo avec une paire mixte.

Si les paquets nécessaires ne sont pas compatibles avec Bullseye, préparer une
image cohérente au lieu de multiplier les remplacements manuels de bibliothèques.

## Recette sans OpenDroneID

- Démarrage sans erreur et services compagnon disponibles.
- Hotspot/SSH retrouvés après démarrage et après redémarrage complet.
- Radios reconnues par matériel/PHY : Air intégré wlan0, USB wlan1 au relevé ;
  Ground intégré wlan1, USB wlan0 au relevé, sans garantie de stabilité des noms.
- Caméra IMX708 fonctionnelle sur Air, vidéo décodée sur Ground.
- UART FC et OSD conservés, modification des paramètres vérifiée.
- Appairage, fréquence et largeur de canal cohérents.
- Pas de régression de charge, mémoire, température, sous-tension ou latence.
- Aucun module OpenDroneID ajouté ou activé à ce jalon.

Résultats matériels détaillés et spécifications fonctionnelles :
[intégration OpenDroneID](opendroneid-integration-spec-fr.md).