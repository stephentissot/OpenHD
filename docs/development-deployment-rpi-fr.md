# Environnement de développement et déploiement Raspberry Pi

## État de préparation — 17 novembre 2026

Objectif : compiler le fork `2.7-evo` sans modification du code fonctionnel,
puis valider Air et Ground avant l'intégration OpenDroneID.

- Pi : Zero 2 W, Raspbian Bullseye armhf, OpenHD 2.6.4.
- Source de référence : `c75c37e6bcd014a990b81ae9b2556eb6afefd924` ; les commits
  suivants au moment de cet audit ne changent que la documentation.
- Windows : Git disponible ; Docker, CMake et Ninja non détectés dans le PATH.
- WSL2 opérationnel après redémarrage ; Ubuntu 22.04.5 installé.
- Environnement de build sur disque Linux sous `/opt/openhd-dev`, hors OneDrive.
- Chroot Raspberry Pi OS Lite Bullseye du 03/05/2023, ARMhf exécuté par QEMU ;
   GCC 10.2.1, CMake 3.18.4, Ninja, libcamera-openhd **1.2.7** installés.
- Build Release **réussi** du cœur à `1e84d4b8` ; `git diff c75c37e6 -- OpenHD`
   vide : aucune modification du code fonctionnel.
- Build Release **réussi** du compagnon OpenHD-SysUtils, révision
   `e89ce3d8541189c145c949cd04dc5b5f1d62cc83` du dépôt officiel.
- Deux paquets ARMhf reconstruits intégralement avec libcamera 1.2.7 et exportés
   dans `artifacts/rpi-baseline-camera127/`
   (répertoire ignoré par Git), avec manifestes et SHA-256 vérifiés après copie.
- Aucun déploiement 2.7 réalisé ; aucun service du Pi modifié.

**Étape actuelle : audit des dépendances et de la migration SysUtils sur le Pi
Air, puis sauvegarde et préparation d'un retour arrière autonome.** Un build
réussi n'est pas une validation matérielle.

### Artefacts de référence produits

| Paquet | Version | SHA-256 |
| --- | --- | --- |
| openhd | 2.7.0-1794903066-1e84d4b8 | `710989e5403c8c091116ab8443d6d9c32fc80c95c3def224a4e3247cd5fcbf09` |
| openhd-sys-utils | 1.0.3.20261117093806 | `9705b61999b59eb0df450f0b215ccca9bcf68168cd9061b74190fe688be1b56a` |

Ces empreintes remplacent le premier jeu construit avec libcamera 1.2.10,
conservé séparément dans `artifacts/rpi-baseline/`. Ne pas déployer ce premier
jeu. Après changement de version d'une bibliothèque système, reconstruire avec
`OPENHD_CLEAN_BUILD=1` : les dates anciennes des fichiers de paquets peuvent
empêcher Ninja de détecter seul le changement.

Les deux exécutables sont ELF 32 bits ARM EABI5, chargeur ARMhf ; aucune
bibliothèque manquante dans le chroot selon `ldd`. Ce contrôle reste à répéter
sur chaque Pi. SysUtils n'a pas été lancé : son démarrage peut configurer le
matériel, tenter un redimensionnement de partition et redémarrer le système.

Image officielle téléchargée depuis `downloads.raspberrypi.com`, empreinte
SHA-256 vérifiée :
`b5e3a1d984a7eaa402a6e078d707b506b962f6804d331dcc0daa61debae3a19a`.
Racine extraite depuis la partition commençant au secteur 532480 (512 octets),
image montée en lecture seule puis copiée avec préservation des permissions.

### Outillage local validé

- [Provisionnement](../tools/dev-rpi/provision-armhf.sh) : à lancer en root
   dans Ubuntu WSL, uniquement après extraction de l'image dans
   `/opt/openhd-dev/armhf-rootfs` et clonage des deux dépôts dans `source` et
   `sysutils-source`. Ne jamais lancer sur un Pi.
- [Build Release](../tools/dev-rpi/build-armhf.sh) : configure et compile dans
   `/build` du chroot, quatre tâches parallèles, journaux dans
   `/opt/openhd-dev/logs`. Rétablit les montages après extinction de WSL.
- [Packaging](../tools/dev-rpi/package-armhf.sh) : construit le paquet OpenHD
   avec `dpkg-deb`, mêmes hooks, unité et dépendances que le packaging upstream
   `standard armhf`, sans relancer son build en source. Produit aussi le paquet
   CPack officiel de SysUtils et les manifestes de versions/bibliothèques.
- [Garde anti-services](../tools/dev-rpi/policy-rc.d) : bloque les démarrages de
   services pendant les installations APT du chroot ; normalisé en LF à la copie.

Les sources fonctionnelles du fork et de SysUtils restent inchangées. Ce chroot
est une isolation de build/ABI, pas une barrière de sécurité : ne pas y exécuter
les démons de contrôle matériel ni des scripts non examinés.

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

## Procédure de préparation

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

Le compagnon officiel est <https://github.com/OpenHD/OpenHD-SysUtils>.
Le dépôt APT release Bullseye propose SysUtils 1.0.0, tandis que le dépôt source
audité est en 1.0.3 : ne pas présumer que le candidat APT satisfait le protocole.
Le paquet compagnon local est donc préparé séparément, sans l'installer.
Le socket attendu par le cœur est `/run/openhd/openhd_sys.sock`.
Sans ce compagnon, l'inventaire Wi-Fi est vide et la plateforme peut rester
inconnue ; les anciens marqueurs Air/Ground seuls ne suffisent plus.

Avant le premier démarrage de SysUtils, examiner sa configuration et la logique
de premier boot : synchronisation des paramètres, configuration caméra,
montages et redimensionnement éventuel FAT32. Ne pas simplement démarrer ce
service sur l'image 2.6.4 sans préparation et sauvegarde des réglages de boot.

### Audit SSH Air complémentaire

- Authentification réussie ; `sudo -n true` réussit sans mot de passe.
- Ancien paquet SysUtils : `0.4-12-01-2024--13-19-56`, unité activée
   `openhd_sys_utils.service` (underscores). Le nouveau paquet utilise
   `openhd-sys-utils.service` (tirets) : éviter deux orchestrateurs simultanés.
- `/run/openhd` absent : aucun socket du protocole 2.7 disponible au relevé.
- libcamera-openhd **1.2.7**, gst-openhd-plugins `2.3-evo-03031839`, libc6
   `2.31-13+rpt2+rpi1+deb11u5`, libstdc++6 `10.2.1-6+rpi1`.
- QOpenHD `2.6.4-05-22-2025--17-28-18-3d325016`, service désactivé sur Air.
- Racine : 712 Mio libres ; boot : 219 Mio libres. Aucun paquet installé.
- Attention à la casse : la configuration **nouveau SysUtils** est
   `/usr/local/share/OpenHD/SysUtils/config.json`, distincte du chemin historique
   `/usr/local/share/openhd` du cœur. Le chemin en minuscules testé au premier
   inventaire ne démontre pas à lui seul l'absence de configuration en majuscules.
- Les nouveaux imports de rôle/caméra utilisent `/Config`, non les seuls
   marqueurs historiques `/boot/openhd`. Préparer explicitement la migration
   à partir des réglages réellement présents, sans créer un faux adaptateur.

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

## Première transaction Air : échec puis rollback validé

Une tentative de migration Air seule a été lancée depuis la session SSH avec
`systemd-run`, indépendamment de la connexion. Le Ground reste inchangé.
Le retour de la commande de lancement n'a pas été reçu : l'arrêt du cœur a
coupé SSH et le hotspot n'était plus annoncé lors du premier contrôle.
**Ne pas considérer cette tentative comme une installation réussie.**

Préparations effectivement validées sur la cible :

- Deux anciens paquets reconstruits pour le retour en 2.6.4 et ancien SysUtils,
   archive des réglages/boot/unités/profils réseau, sommes SHA256 vérifiées.
- Copie privée sur le PC dans les artefacts ignorés ; SHA256 de cette archive :
   `04aa75b5ff740781d4297d8d99ab35c3fea14cd49ae2027e6df2e2b1502eb65f`.
   Elle contient des profils réseau et doit rester privée, hors Git.
- `ldd` des deux nouveaux exécutables sans bibliothèque manquante sur le Pi ;
   simulation APT : deux remplacements, aucune nouvelle dépendance.
- Tests locaux de conservation UART/caméra/puissance radio, idempotence de la
   migration JSON et syntaxe Bash/Perl réussis.

Les outils de transaction arment un timer persistant avant tout arrêt de service :
retour arrière après 15 minutes sans confirmation, nouvelles tentatives après
échec. Son exécution réelle a ensuite été confirmée par le retour SSH et les
versions restaurées décrites ci-dessous. Ne pas couper l'alimentation pendant
une transaction. Le script de restauration est
[air-rollback.sh](../tools/dev-rpi/air-rollback.sh).

Précautions de migration : `firstboot=false` dans la configuration du nouveau
compagnon (son firstboot peut reformater la partition vidéo), montages bind
compatibles sans repartitionnement, ancien fichier de réserve conservé par
renommage, démarrage automatique bloqué pendant l'installation des paquets.
Les sources fonctionnelles OpenHD et SysUtils restent inchangées.

Incident de sauvegarde identifié et corrigé : `dpkg-repack` peut retourner un
statut global de succès malgré l'échec d'un paquet. L'ancien postinst SysUtils
avait supprimé un conffile X20, ce qui empêchait de reconstruire son paquet.
La reconstruction omet uniquement ce conffile déjà absent ; les deux DEB de
retour arrière ont ensuite été vérifiés. Le précontrôle impose désormais leur
présence explicite.

Contrôles requis avant toute confirmation : relire les journaux de transaction et de restauration,
versions et états des paquets/services, socket compagnon, hotspot, IMX708 et UART.
Ne désarmer le timer qu'après validation, pas sur la seule présence des nouveaux
paquets. Aucun test de paire vidéo ni développement OpenDroneID n'est validé ici.

### Récupération confirmée après cette tentative

Le hotspot est réapparu et SSH a été rétabli vers 11 h 33 (heure PC).
Le journal porte `ROLLBACK_COMPLETE_264_RESTORED` : paquets OpenHD
`2.6.4-202505221612-d9ed491` et SysUtils `0.4-12-01-2024--13-19-56`
restaurés. `dpkg --audit` ne rapporte rien ; OpenHD, SSH et NetworkManager sont
actifs. Le timer de récupération est désactivé après sa réussite. La radio USB
est à nouveau monitor sur le canal 1 ; ce constat ne valide pas le lien vidéo.

La transaction d'installation avait bien terminé, puis le cœur 2.7 redémarrait
en boucle : `SIGSEGV` et exception `vector::_M_range_check` (index 1, taille 1).
Une erreur locale de migration est identifiée : les quatre vecteurs de puissance
étaient créés avec une seule entrée. Le cœur expose systématiquement
`MAX_WIFI_CARDS=4` paramètres, indépendamment du nombre de radios connectées.
Le script fournit désormais quatre entrées conservant les valeurs historiques ;
le test vérifie explicitement cette taille et le dernier emplacement. Cette
correction ne démontre pas à elle seule la résolution des `SIGSEGV`.

Le journal NetworkManager montre aussi la suppression du profil hotspot pendant
son activation (`connection-removed`). Les redémarrages et destructions d'objets
hotspot peuvent expliquer cette disparition ; l'ordre causal complet reste à
vérifier. Aucune seconde installation n'avait encore été lancée à ce stade.

## Second essai Air : démarrage 2.7 confirmé le 17 novembre 2026

Après accord utilisateur, le second essai a utilisé **exactement les mêmes
binaires non modifiés**, avec la migration corrigée des quatre emplacements de
puissance. Le helper [air-prepare-retry.sh](../tools/dev-rpi/air-prepare-retry.sh)
a vérifié la restauration 2.6.4 et les sommes des sauvegardes, puis archivé les
journaux et marqueurs du premier essai sous `attempts/first` sur la cible.
La configuration de sécurité conservée par le rollback a été acceptée seulement
après vérification JSON de `firstboot=false`.

Lancement détaché vers 11 h 41, reconnexion au hotspot vers 11 h 43 (heure PC).
Le journal d'installation atteint `AIR_27_INSTALLED_ROLLBACK_STILL_ARMED`.
Contrôles réellement effectués avant confirmation :

- Paquets : OpenHD `2.7.0-1794903066-1e84d4b8`, SysUtils
   `1.0.3.20261117093806` ; le cœur affiche **2.7.1-evo**.
- Deux services actifs, `NRestarts=0`, même PID cœur observé pendant plus de
   huit minutes ; aucun nouveau SIGSEGV ni exception de vecteur dans ce démarrage.
   Les anciennes erreurs restent présentes dans le journal historique.
- SysUtils annonce `READY`, socket compagnon présent ; radios détectées
   `wlan0/BROADCOM` et `wlan1/OPENHD_RTL_88X2EU`.
- Hotspot `openhd_air`, adresse 192.168.3.1, SSH accessibles ; USB en monitor.
- Quatre vecteurs de puissance présents, chacun de longueur quatre ; réglages
   hérités conservés (25 mW, index 10, MCS 2, largeur 20 MHz).
- IMX708/encodeur effectivement ouverts par le cœur. Le journal de télémétrie
   produit des statistiques vivantes : débit encodé 11 073 962 bit/s, débit injecté
   13 833 095 bit/s avec FEC 20 %, 1 359 paquets/s, zéro image abandonnée et zéro
   paquet TX abandonné dans l'échantillon. Cela prouve une activité d'encodage/
   injection déclarée par le cœur, **pas une réception ni une image décodée**.
- UART `/dev/ttyS0` ouvert, mais avertissement initial « FC connected ? » :
   aucun échange MAVLink réel avec la FC n'est démontré par ces contrôles.
- RAM disponible environ 253 MiB sur 426 MiB, charge processus environ 138 %
   (plusieurs cœurs), température SoC environ 66,6 °C, `throttled=0x0`.
   `dpkg --audit` sans anomalie. Ces relevés ne constituent pas un essai d'endurance.

Attention à la fréquence : les réglages et statistiques du cœur indiquent
5745 MHz, mais `iw dev` rapporte toujours canal 1 / 2412 MHz sur l'USB, comme
avant migration. **La fréquence RF effective n'est donc pas validée** ; aucune
fréquence n'a été changée pour masquer cette divergence.

Le marqueur `deployment-confirmed` a été créé sous le même verrou `flock` que
la restauration, après vérification de l'absence de rollback en cours, des
versions et de la santé des services. Le timer a ensuite été désactivé ; sa
désactivation et les deux services toujours actifs sans redémarrage ont été
relus. La sauvegarde privée et les paquets de retour restent conservés.

**Limite de ce jalon : installation et démarrage Air validés, recette de paire
non terminée.** Aucun redémarrage complet n'a encore été testé, aucune réception
vidéo Ground ni OSD/FC n'a été validée. Le Ground reste en 2.6.4 et n'a pas été
modifié. Aucun développement ni émission OpenDroneID n'a été ajouté.