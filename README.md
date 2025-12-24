# reShop
A Nintendo 3DS eShop remake built with libctru.



[Check our website!](https://unitendo.jumpingcrab.com/reShop)


[Join our Discord!](https://unitendo.jumpingcrab.com/reShop/discord)


[![](https://dcbadge.limes.pink/api/server/dCSgz7KERv)](https://unitendo.jumpingcrab.com/reShop/discord)

<img alt="gitleaks badge" src="https://img.shields.io/badge/protected%20by-gitleaks-blue">


> [!NOTE]
> This project is not finished, but it is in active development.

> [!CAUTION]
> This software is still in development. Ensure you back up your SysNAND before even trying to use this.

## v1 Checklist
- [x] License
- [x] Basic framework 
- [x] Basic rendering
- [x] Streamed Audio (Using @VirtuallyExisting's custom implementation)
- [x] httpc downloading based off of devkitpro's example
- [x] CIA Installation from SD Card
- [x] Grabbing listings, descriptions, and icons off of a server
- [x] Downloading and Installing apps
- [x] Downloading Animation
- [x] Closed Beta
- [ ] Complete GUI
- [ ] eShop-ify the entire interface (make it look as similar as possible)
- [ ] Open Beta

## v2 Checklist
- [ ] Messaging (like Juxt communities but realtime) under each app listing
- [ ] A user rating system
- [ ] Download DS apps as well (not just DSiWare!)
- [ ] Fake Currency (not needed to get homebrew, more like an achievement system. you can get different ranks on a leaderboard, etc.)
- [ ] Upgraded Music Options (integrates with the currency)
- [ ] Closed Beta
- [ ] Implement friending users (friends list and in-app)
- [ ] Suggest users who play similar games as you
- [ ] Add downloading from other sources
- [ ] Open Beta


## How to compile
You'll need devKitPro, 3ds-dev, and 3ds-opusfile. To install the latter two run
```bash
sudo dkp-pacman -S 3ds-dev 3ds-opusfile
```
Depending on your platform, you may not need sudo or dkp-.
Then, run the following commands:
```bash
git clone https://github.com/Unitendo/reShop/
cd reShop
make
```
However, you will not be able to connect to our servers, seeing as that the proper identification is private and inaccessible to the public.

Therefore, the best way to get builds is to wait for us to compile a build and publish it ourselves. Thank you for your patience.



