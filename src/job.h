#ifndef JOB_H
#define JOB_H

#include <stdbool.h>

enum JobType {
    JOB_TYPE_EXPLORER = 0,
    JOB_TYPE_CYGNUS = 1,
    JOB_TYPE_LEGEND = 2,
};

enum Job {
    JOB_BEGINNER = 0,

    JOB_SWORDSMAN = 100,
        JOB_FIGHTER = 110,
        JOB_CRUSADER,
        JOB_HERO,

        JOB_PAGE = 120,
        JOB_WHITE_KNGIHT,
        JOB_PALADIN,

        JOB_SPEARMAN = 130,
        JOB_BERSERKER,
        JOB_DARK_KNGIHT,

    JOB_MAGICIAN = 200,
        JOB_FIRE_WIZARD = 210,
        JOB_FIRE_MAGE,
        JOB_FIRE_ARCH_MAGE,

        JOB_ICE_WIZARD = 220,
        JOB_ICE_MAGE,
        JOB_ICE_ARCH_MAGE,

        JOB_CLERIC = 230,
        JOB_PRIEST,
        JOB_BISHOP,

    JOB_ARCHER = 300,
        JOB_HUNTER = 310,
        JOB_RANGER,
        JOB_BOW_MASTER,

        JOB_CROSSBOWMAN = 320,
        JOB_SNIPER,
        JOB_MARKSMAN,

    JOB_ROGUE = 400,
        JOB_ASSASSIN = 410,
        JOB_HERMIT,
        JOB_NIGHT_LORD,

        JOB_BANDIT = 420,
        JOB_CHIEF_BANDIT,
        JOB_SHADOWER,

    JOB_PIRATE = 500,
        JOB_BRAWLER = 510,
        JOB_MARAUDER,
        JOB_BUCCANEER,

        JOB_GUNSLINGER = 520,
        JOB_OUTLAW,
        JOB_CORSAIR,

    JOB_GM = 900,
        JOB_SUPER_GM = 910,

    JOB_NOBLESSE = 1000,

    JOB_DAWN_WARRIOR = 1100,
        JOB_DAWN_WARRIOR_1 = 1110,
        JOB_DAWN_WARRIOR_2,
        JOB_DAWN_WARRIOR_3,

    JOB_BLAZE_WIZARD = 1200,
        JOB_BLAZE_WIZARD_1 = 1210,
        JOB_BLAZE_WIZARD_2,
        JOB_BLAZE_WIZARD_3,

    JOB_WIND_ARCHER = 1300,
        JOB_WIND_ARCHER_1 = 1310,
        JOB_WIND_ARCHER_2,
        JOB_WIND_ARCHER_3,

    JOB_NIGHT_WALKER = 1400,
        JOB_NIGHT_WALKER_1 = 1410,
        JOB_NIGHT_WALKER_2,
        JOB_NIGHT_WALKER_3,

    JOB_THUNDER_BREAKER = 1500,
        JOB_THUNDER_BREAKER_1 = 1510,
        JOB_THUNDER_BREAKER_2,
        JOB_THUNDER_BREAKER_3,

    JOB_LEGEND = 2000,
        JOB_EVAN,

    JOB_ARAN = 2100,
        JOB_ARAN_1 = 2110,
        JOB_ARAN_2,
        JOB_ARAN_3,

    JOB_EVAN_1 = 2200,
        JOB_EVAN_2 = 2210,
        JOB_EVAN_3,
        JOB_EVAN_4,
        JOB_EVAN_5,
        JOB_EVAN_6,
        JOB_EVAN_7,
        JOB_EVAN_8,
        JOB_EVAN_9,
        JOB_EVAN_10,
};

static inline enum JobType job_type(enum Job job)
{
    return job / 1000;
}

static inline bool job_is_a(enum Job job, enum Job base)
{
    return (job / 10 == base / 10 && job >= base) || ((base / 10) % 10 == 0 && job / 100 == base / 100);
}

#endif

