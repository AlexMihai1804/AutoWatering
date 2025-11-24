#ifndef WATERING_H
#define WATERING_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/drivers/gpio.h>

/* Forward declarations for enhanced database types */
struct plant_full_data_t;
struct soil_enhanced_data_t;
struct irrigation_method_data_t;
/* Forward declaration for water_balance_t */
struct water_balance_t;

/**
 * @file watering.h
 * @brief Main interface for the automatic irrigation system
 * 
 * This header defines the public API and data structures for controlling 
 * a multi-channel irrigation system with flow monitoring capabilities.
 * 
 * PERFORMANCE IMPROVEMENTS:
 * - Removed throttling delays for fast history channel switching
 * - Added mutex timeouts to prevent system freezes
 * - Optimized cache timeouts for responsive channel changes
 */

/** Number of available watering channels in the system */
#define WATERING_CHANNELS_COUNT 8

/**
 * @brief Standardized error codes for watering system
 */
typedef enum {
    WATERING_SUCCESS = 0,                /**< Operation completed successfully */
    WATERING_ERROR_INVALID_PARAM = -1,   /**< Invalid parameter provided */
    WATERING_ERROR_NOT_INITIALIZED = -2, /**< System not initialized */
    WATERING_ERROR_HARDWARE = -3,        /**< Hardware failure */
    WATERING_ERROR_BUSY = -4,            /**< System busy with another operation */
    WATERING_ERROR_QUEUE_FULL = -5,      /**< Task queue is full */
    WATERING_ERROR_TIMEOUT = -6,         /**< Operation timed out */
    WATERING_ERROR_CONFIG = -7,          /**< Configuration error */
    WATERING_ERROR_RTC_FAILURE = -8,     /**< RTC communication failure */
    WATERING_ERROR_STORAGE = -9,         /**< Storage operation failed */
    WATERING_ERROR_DATA_CORRUPT = -10,
    WATERING_ERROR_INVALID_DATA = -11,
    WATERING_ERROR_BUFFER_FULL = -12,
    WATERING_ERROR_NO_MEMORY = -13,
} watering_error_t;

/**
 * @brief Schedule type for automatic watering
 * 
 * Defines how watering events are scheduled over time.
 */
typedef enum { 
    SCHEDULE_DAILY,    /**< Water on specific days of the week */
    SCHEDULE_PERIODIC  /**< Water every N days */
} schedule_type_t;

/**
 * @brief Watering mode that determines how irrigation is measured
 */
typedef enum watering_mode { 
    WATERING_BY_DURATION,         /**< Water for a specific time duration */
    WATERING_BY_VOLUME,           /**< Water until a specific volume is reached */
    WATERING_AUTOMATIC_QUALITY,   /**< Automatic mode: 100% of calculated requirement */
    WATERING_AUTOMATIC_ECO        /**< Automatic mode: 70% of calculated requirement for water conservation */
} watering_mode_t;

/**
 * @brief Watering system state machine states
 */
typedef enum {
    WATERING_STATE_IDLE,           /**< No active watering */
    WATERING_STATE_WATERING,       /**< Actively watering */
    WATERING_STATE_PAUSED,         /**< Watering temporarily paused */
    WATERING_STATE_ERROR_RECOVERY, /**< Error recovery in progress */
} watering_state_t;

/**
 * @brief Trigger types for watering events
 */
typedef enum {
    WATERING_TRIGGER_MANUAL = 0,
    WATERING_TRIGGER_SCHEDULED = 1,
    WATERING_TRIGGER_REMOTE = 2
} watering_trigger_type_t;

/**
 * @brief Skip reasons for watering tasks
 */
typedef enum {
    WATERING_SKIP_REASON_RAIN = 0,
    WATERING_SKIP_REASON_MOISTURE = 1,
    WATERING_SKIP_REASON_MANUAL = 2,
    WATERING_SKIP_REASON_ERROR = 3
} watering_skip_reason_t;

/**
 * @brief Type of plant being grown in the channel
 */
typedef enum {
    PLANT_TYPE_VEGETABLES,     /**< Vegetables (tomatoes, peppers, cucumbers, etc.) */
    PLANT_TYPE_HERBS,          /**< Herbs (basil, parsley, thyme, etc.) */
    PLANT_TYPE_FLOWERS,        /**< Flowers (roses, tulips, marigolds, etc.) */
    PLANT_TYPE_SHRUBS,         /**< Shrubs and bushes */
    PLANT_TYPE_TREES,          /**< Trees and large plants */
    PLANT_TYPE_LAWN,           /**< Grass and lawn areas */
    PLANT_TYPE_SUCCULENTS,     /**< Succulents and cacti */
    PLANT_TYPE_OTHER           /**< Other plant types (with custom name) */
} plant_type_t;

/**
 * @brief Specific vegetable types for detailed plant management
 */
typedef enum {
    VEGETABLE_TOMATOES,        /**< Tomatoes (regular, cherry, etc.) */
    VEGETABLE_PEPPERS,         /**< Peppers (sweet, hot, bell) */
    VEGETABLE_CUCUMBERS,       /**< Cucumbers and gherkins */
    VEGETABLE_LETTUCE,         /**< Lettuce and leafy greens */
    VEGETABLE_CARROTS,         /**< Carrots and root vegetables */
    VEGETABLE_ONIONS,          /**< Onions, garlic, scallions */
    VEGETABLE_BEANS,           /**< Beans (green, lima, etc.) */
    VEGETABLE_PEAS,            /**< Peas and legumes */
    VEGETABLE_SQUASH,          /**< Squash, zucchini, pumpkins */
    VEGETABLE_BROCCOLI,        /**< Broccoli, cauliflower, cabbage */
    VEGETABLE_EGGPLANT,        /**< Eggplant and aubergine */
    VEGETABLE_SPINACH,         /**< Spinach and similar greens */
    VEGETABLE_RADISHES,        /**< Radishes and turnips */
    VEGETABLE_CORN,            /**< Sweet corn */
    VEGETABLE_POTATO,          /**< Potatoes */
    VEGETABLE_SWEET_POTATO,    /**< Sweet potatoes and yams */
    VEGETABLE_BEETS,           /**< Beetroot and sugar beets */
    VEGETABLE_KALE,            /**< Kale and collard greens */
    VEGETABLE_SWISS_CHARD,     /**< Swiss chard and rainbow chard */
    VEGETABLE_ARUGULA,         /**< Arugula and rocket */
    VEGETABLE_BOK_CHOY,        /**< Bok choy and Asian greens */
    VEGETABLE_ASPARAGUS,       /**< Asparagus spears */
    VEGETABLE_ARTICHOKES,      /**< Globe artichokes */
    VEGETABLE_BRUSSELS_SPROUTS, /**< Brussels sprouts */
    VEGETABLE_LEEKS,           /**< Leeks and baby leeks */
    VEGETABLE_CELERY,          /**< Celery and celeriac */
    VEGETABLE_FENNEL,          /**< Florence fennel */
    VEGETABLE_OKRA,            /**< Okra pods */
    VEGETABLE_TURNIPS,         /**< Turnips and rutabaga */
    VEGETABLE_PARSNIPS,        /**< Parsnips */
    VEGETABLE_KOHLRABI,        /**< Kohlrabi */
    VEGETABLE_WATERCRESS,      /**< Watercress and garden cress */
    VEGETABLE_ENDIVE,          /**< Endive and chicory */
    VEGETABLE_RADICCHIO,       /**< Radicchio and red chicory */
    VEGETABLE_RHUBARB,         /**< Rhubarb stalks */
    VEGETABLE_HORSERADISH,     /**< Horseradish root */
    VEGETABLE_JERUSALEM_ARTICHOKE, /**< Sunchokes */
    VEGETABLE_JICAMA,          /**< Mexican turnip */
    VEGETABLE_TOMATILLO,       /**< Husk cherry */
    VEGETABLE_GROUND_CHERRY,   /**< Cape gooseberry */
    VEGETABLE_PEPINO_MELON,    /**< Sweet cucumber */
    VEGETABLE_CHAYOTE,         /**< Vegetable pear */
    VEGETABLE_BITTER_MELON,    /**< Bitter gourd */
    VEGETABLE_DRAGON_FRUIT,    /**< Pitaya cactus fruit */
    VEGETABLE_MUSHROOM,        /**< Edible mushroom varieties */
    VEGETABLE_MICROGREENS,     /**< Microgreen varieties */
    VEGETABLE_SPROUTS,         /**< Bean and seed sprouts */
    VEGETABLE_OTHER            /**< Other vegetables */
} vegetable_type_t;

/**
 * @brief Specific herb types for detailed management
 */
typedef enum {
    HERB_BASIL,                /**< Sweet basil, Thai basil */
    HERB_PARSLEY,              /**< Flat-leaf, curly parsley */
    HERB_CILANTRO,             /**< Cilantro/Coriander */
    HERB_THYME,                /**< Common thyme, lemon thyme */
    HERB_ROSEMARY,             /**< Rosemary */
    HERB_OREGANO,              /**< Oregano and marjoram */
    HERB_SAGE,                 /**< Common sage */
    HERB_CHIVES,               /**< Chives */
    HERB_DILL,                 /**< Dill weed */
    HERB_MINT,                 /**< Peppermint, spearmint */
    HERB_LAVENDER,             /**< Culinary lavender */
    HERB_CHERVIL,              /**< Chervil */
    HERB_TARRAGON,             /**< French tarragon */
    HERB_BAY_LEAVES,           /**< Bay laurel */
    HERB_LEMON_BALM,           /**< Lemon balm */
    HERB_LEMONGRASS,           /**< Lemongrass */
    HERB_MARJORAM,             /**< Sweet marjoram */
    HERB_SAVORY,               /**< Summer and winter savory */
    HERB_FENNEL,               /**< Fennel fronds and bulb */
    HERB_ANISE,                /**< Anise hyssop */
    HERB_BORAGE,               /**< Borage flowers and leaves */
    HERB_CATNIP,               /**< Catnip and catmint */
    HERB_CHAMOMILE,            /**< German and Roman chamomile */
    HERB_LEMON_VERBENA,        /**< Lemon verbena */
    HERB_STEVIA,               /**< Stevia sweetener plant */
    HERB_PEPPERMINT,           /**< Peppermint varieties */
    HERB_SPEARMINT,            /**< Spearmint varieties */
    HERB_CHOCOLATE_MINT,       /**< Chocolate mint */
    HERB_APPLE_MINT,           /**< Apple mint */
    HERB_BERGAMOT,             /**< Wild bergamot, bee balm */
    HERB_COMFREY,              /**< Comfrey (medicinal) */
    HERB_ECHINACEA,            /**< Purple coneflower */
    HERB_VALERIAN,             /**< Valerian root */
    HERB_ELDERFLOWER,          /**< Elderflower blooms */
    HERB_NETTLE,               /**< Stinging nettle */
    HERB_DANDELION,            /**< Dandelion greens */
    HERB_PLANTAIN,             /**< Plantain leaves */
    HERB_WILD_GARLIC,          /**< Ramps, wild leeks */
    HERB_SORREL,               /**< French sorrel */
    HERB_PURSLANE,             /**< Portulaca oleracea */
    HERB_LAMB_QUARTERS,        /**< Chenopodium album */
    HERB_CALENDULA,            /**< Pot marigold (edible) */
    HERB_NASTURTIUM,           /**< Edible nasturtium */
    HERB_VIOLET,               /**< Sweet violet leaves */
    HERB_CHICKWEED,            /**< Stellaria media */
    HERB_CLEAVERS,             /**< Galium aparine */
    HERB_WOOD_SORREL,          /**< Oxalis species */
    HERB_MALLOW,               /**< Common mallow */
    HERB_ROSE_GERANIUM,        /**< Pelargonium graveolens */
    HERB_LEMON_THYME,          /**< Citrus thyme */
    HERB_MEXICAN_MINT_MARIGOLD, /**< Tagetes lucida */
    HERB_EPAZOTE,              /**< Dysphania ambrosioides */
    HERB_SHISO,                /**< Japanese basil */
    HERB_PERILLA,              /**< Korean perilla */
    HERB_OTHER                 /**< Other herbs */
} herb_type_t;

/**
 * @brief Specific flower types for garden management
 */
typedef enum {
    FLOWER_ROSES,              /**< Garden roses, climbing roses */
    FLOWER_TULIPS,             /**< Spring tulips */
    FLOWER_DAFFODILS,          /**< Daffodils and narcissus */
    FLOWER_MARIGOLDS,          /**< French, African marigolds */
    FLOWER_PETUNIAS,           /**< Wave, grandiflora petunias */
    FLOWER_IMPATIENS,          /**< Busy lizzie, balsam */
    FLOWER_BEGONIAS,           /**< Wax, tuberous begonias */
    FLOWER_GERANIUMS,          /**< Zonal, ivy geraniums */
    FLOWER_PANSIES,            /**< Pansies and violas */
    FLOWER_SUNFLOWERS,         /**< Annual sunflowers */
    FLOWER_ZINNIAS,            /**< Common zinnias */
    FLOWER_COSMOS,             /**< Garden cosmos */
    FLOWER_NASTURTIUMS,        /**< Climbing nasturtiums */
    FLOWER_DAHLIAS,            /**< Border, dinner plate dahlias */
    FLOWER_LILIES,             /**< Asiatic, Oriental lilies */
    FLOWER_IRIS,               /**< Bearded, Siberian iris */
    FLOWER_PEONIES,            /**< Herbaceous peonies */
    FLOWER_HYDRANGEAS,         /**< Bigleaf, panicle hydrangeas */
    FLOWER_CHRYSANTHEMUMS,     /**< Garden mums, football mums */
    FLOWER_ASTERS,             /**< Fall asters, New England asters */
    FLOWER_HOLLYHOCKS,         /**< Tall hollyhocks */
    FLOWER_LAVENDER,           /**< English, French lavender */
    FLOWER_SALVIA,             /**< Annual and perennial salvia */
    FLOWER_VERBENA,            /**< Garden verbena */
    FLOWER_SNAPDRAGONS,        /**< Tall and dwarf snapdragons */
    FLOWER_CALENDULA,          /**< Pot marigold */
    FLOWER_BACHELOR_BUTTONS,   /**< Cornflower */
    FLOWER_SWEET_PEAS,         /**< Climbing sweet peas */
    FLOWER_MORNING_GLORY,      /**< Climbing morning glory */
    FLOWER_MOONFLOWER,         /**< Night-blooming moonflower */
    FLOWER_FOUR_OCLOCK,        /**< Four o'clock flowers */
    FLOWER_PORTULACA,          /**< Moss rose */
    FLOWER_CELOSIA,            /**< Cockscomb celosia */
    FLOWER_COLEUS,             /**< Ornamental coleus */
    FLOWER_CALADIUM,           /**< Fancy-leaved caladium */
    FLOWER_GLADIOLUS,          /**< Sword lilies */
    FLOWER_CANNA,              /**< Canna lilies */
    FLOWER_ELEPHANT_EAR,       /**< Colocasia, Alocasia */
    FLOWER_HIBISCUS,           /**< Tropical hibiscus */
    FLOWER_BOUGAINVILLEA,      /**< Climbing bougainvillea */
    FLOWER_JASMINE,            /**< Star jasmine, Confederate jasmine */
    FLOWER_GARDENIA,           /**< Cape jasmine */
    FLOWER_AZALEA,             /**< Deciduous azaleas */
    FLOWER_CAMELLIA,           /**< Camellia sasanqua */
    FLOWER_RHODODENDRON,       /**< Rhododendron species */
    FLOWER_MAGNOLIA,           /**< Magnolia blooms */
    FLOWER_WISTERIA,           /**< Climbing wisteria */
    FLOWER_CLEMATIS,           /**< Clematis vines */
    FLOWER_HONEYSUCKLE,        /**< Lonicera varieties */
    FLOWER_TRUMPET_VINE,       /**< Campsis radicans */
    FLOWER_PASSION_FLOWER,     /**< Passiflora varieties */
    FLOWER_MANDEVILLA,         /**< Climbing mandevilla */
    FLOWER_FUCHSIA,            /**< Hanging fuchsias */
    FLOWER_LANTANA,            /**< Lantana camara */
    FLOWER_PENTAS,             /**< Egyptian star cluster */
    FLOWER_VINCA,              /**< Madagascar periwinkle */
    FLOWER_TORENIA,            /**< Wishbone flower */
    FLOWER_ANGELONIA,          /**< Summer snapdragon */
    FLOWER_CLEOME,             /**< Spider flower */
    FLOWER_AMARANTHUS,         /**< Love-lies-bleeding */
    FLOWER_GOMPHRENA,          /**< Globe amaranth */
    FLOWER_STRAWFLOWER,        /**< Helichrysum */
    FLOWER_STATICE,            /**< Limonium sinuatum */
    FLOWER_NIGELLA,            /**< Love-in-a-mist */
    FLOWER_NICOTIANA,          /**< Flowering tobacco */
    FLOWER_BROWALLIA,          /**< Bush violet */
    FLOWER_LOBELIA,            /**< Edging lobelia */
    FLOWER_ALYSSUM,            /**< Sweet alyssum */
    FLOWER_CANDYTUFT,          /**< Iberis varieties */
    FLOWER_DIANTHUS,           /**< Carnations, pinks */
    FLOWER_OTHER               /**< Other flowers */
} flower_type_t;

/**
 * @brief Specific tree types for orchard and landscape management
 */
typedef enum {
    TREE_CITRUS,               /**< Lemon, orange, lime trees */
    TREE_APPLE,                /**< Apple varieties */
    TREE_PEAR,                 /**< Pear varieties */
    TREE_CHERRY,               /**< Sweet and sour cherries */
    TREE_PLUM,                 /**< Plum and damson */
    TREE_PEACH,                /**< Peach and nectarine */
    TREE_FIG,                  /**< Fig trees */
    TREE_OLIVE,                /**< Olive trees */
    TREE_AVOCADO,              /**< Avocado trees */
    TREE_MAPLE,                /**< Ornamental maples */
    TREE_OAK,                  /**< Oak varieties */
    TREE_PINE,                 /**< Pine and conifers */
    TREE_PALM,                 /**< Palm trees */
    TREE_WILLOW,               /**< Weeping willows */
    TREE_BIRCH,                /**< Paper birch, river birch */
    TREE_MAGNOLIA,             /**< Southern, star magnolia */
    TREE_DOGWOOD,              /**< Flowering dogwood */
    TREE_REDBUD,               /**< Eastern redbud */
    TREE_CRAPE_MYRTLE,         /**< Crape myrtle varieties */
    TREE_JAPANESE_MAPLE,       /**< Japanese maple cultivars */
    TREE_FRUIT_CITRUS_LEMON,   /**< Lemon trees specifically */
    TREE_FRUIT_CITRUS_ORANGE,  /**< Orange trees specifically */
    TREE_FRUIT_CITRUS_LIME,    /**< Lime trees specifically */
    TREE_FRUIT_CITRUS_GRAPEFRUIT, /**< Grapefruit trees */
    TREE_FRUIT_STONE_APRICOT,  /**< Apricot trees */
    TREE_FRUIT_STONE_PERSIMMON, /**< Persimmon trees */
    TREE_FRUIT_POMEGRANATE,    /**< Pomegranate trees */
    TREE_FRUIT_MANGO,          /**< Mango trees */
    TREE_FRUIT_PAPAYA,         /**< Papaya trees */
    TREE_FRUIT_BANANA,         /**< Banana plants */
    TREE_NUT_ALMOND,           /**< Almond trees */
    TREE_NUT_WALNUT,           /**< Walnut trees */
    TREE_NUT_PECAN,            /**< Pecan trees */
    TREE_NUT_HAZELNUT,         /**< Hazelnut trees */
    TREE_EVERGREEN_SPRUCE,     /**< Spruce varieties */
    TREE_EVERGREEN_FIR,        /**< Fir trees */
    TREE_EVERGREEN_CEDAR,      /**< Cedar varieties */
    TREE_EVERGREEN_JUNIPER,    /**< Juniper trees */
    TREE_SHADE_ELM,            /**< Elm varieties */
    TREE_SHADE_ASH,            /**< Ash trees */
    TREE_SHADE_POPLAR,         /**< Poplar and aspen */
    TREE_SHADE_SYCAMORE,       /**< Sycamore trees */
    TREE_TROPICAL_COCONUT,     /**< Coconut palms */
    TREE_TROPICAL_DATE,        /**< Date palms */
    TREE_FRUIT_LYCHEE,         /**< Lychee trees */
    TREE_FRUIT_LONGAN,         /**< Longan trees */
    TREE_FRUIT_RAMBUTAN,       /**< Rambutan trees */
    TREE_FRUIT_DRAGON_FRUIT,   /**< Dragon fruit cactus */
    TREE_FRUIT_JACKFRUIT,      /**< Jackfruit trees */
    TREE_FRUIT_BREADFRUIT,     /**< Breadfruit trees */
    TREE_FRUIT_STARFRUIT,      /**< Carambola trees */
    TREE_FRUIT_GUAVA,          /**< Guava trees */
    TREE_FRUIT_PASSION_FRUIT,  /**< Passion fruit vines */
    TREE_FRUIT_KIWI,           /**< Kiwi vines */
    TREE_FRUIT_GRAPE,          /**< Grape vines */
    TREE_BERRY_BLUEBERRY,      /**< Blueberry bushes */
    TREE_BERRY_BLACKBERRY,     /**< Blackberry canes */
    TREE_BERRY_RASPBERRY,      /**< Raspberry canes */
    TREE_BERRY_STRAWBERRY,     /**< Strawberry plants */
    TREE_BERRY_ELDERBERRY,     /**< Elderberry bushes */
    TREE_BERRY_GOOSEBERRY,     /**< Gooseberry bushes */
    TREE_BERRY_CURRANT,        /**< Currant bushes */
    TREE_BERRY_CRANBERRY,      /**< Cranberry bogs */
    TREE_FLOWERING_CHERRY,     /**< Ornamental cherry */
    TREE_FLOWERING_PLUM,       /**< Ornamental plum */
    TREE_FLOWERING_PEAR,       /**< Bradford pear */
    TREE_FLOWERING_CRABAPPLE,  /**< Ornamental crabapple */
    TREE_FLOWERING_HAWTHORN,   /**< Hawthorn varieties */
    TREE_CONIFER_REDWOOD,      /**< Coast redwood */
    TREE_CONIFER_SEQUOIA,      /**< Giant sequoia */
    TREE_CONIFER_CYPRESS,      /**< Cypress varieties */
    TREE_CONIFER_ARBORVITAE,   /**< Thuja varieties */
    TREE_CONIFER_HEMLOCK,      /**< Hemlock varieties */
    TREE_BAMBOO,               /**< Bamboo varieties */
    TREE_OTHER                 /**< Other trees */
} tree_type_t;

/**
 * @brief Specific shrub and bush types
 */
typedef enum {
    SHRUB_AZALEA,              /**< Azalea varieties */
    SHRUB_RHODODENDRON,        /**< Rhododendron species */
    SHRUB_HYDRANGEA,           /**< Hydrangea varieties */
    SHRUB_ROSE_BUSH,           /**< Rose bushes */
    SHRUB_BOXWOOD,             /**< Boxwood hedging */
    SHRUB_HOLLY,               /**< Holly bushes */
    SHRUB_JUNIPER,             /**< Juniper shrubs */
    SHRUB_FORSYTHIA,           /**< Forsythia bushes */
    SHRUB_LILAC,               /**< Lilac bushes */
    SHRUB_SPIREA,              /**< Spirea varieties */
    SHRUB_WEIGELA,             /**< Weigela bushes */
    SHRUB_VIBURNUM,            /**< Viburnum species */
    SHRUB_CAMELLIA,            /**< Camellia bushes */
    SHRUB_GARDENIA,            /**< Gardenia bushes */
    SHRUB_MOCK_ORANGE,         /**< Philadelphus varieties */
    SHRUB_BURNING_BUSH,        /**< Euonymus alatus */
    SHRUB_BARBERRY,            /**< Berberis varieties */
    SHRUB_PRIVET,              /**< Ligustrum hedging */
    SHRUB_YEW,                 /**< Taxus varieties */
    SHRUB_ARBORVITAE,          /**< Thuja varieties */
    SHRUB_ELDERBERRY,          /**< Sambucus varieties */
    SHRUB_BUTTERFLY_BUSH,      /**< Buddleia varieties */
    SHRUB_SERVICEBERRY,        /**< Amelanchier varieties */
    SHRUB_NINEBARK,            /**< Physocarpus varieties */
    SHRUB_DOGWOOD_SHRUB,       /**< Shrub dogwood varieties */
    SHRUB_OTHER                /**< Other shrubs */
} shrub_type_t;

/**
 * @brief Specific lawn and grass types
 */
typedef enum {
    LAWN_BERMUDA,              /**< Bermuda grass */
    LAWN_ZOYSIA,               /**< Zoysia grass */
    LAWN_ST_AUGUSTINE,         /**< St. Augustine grass */
    LAWN_KENTUCKY_BLUE,        /**< Kentucky bluegrass */
    LAWN_FESCUE,               /**< Tall fescue, fine fescue */
    LAWN_RYE,                  /**< Perennial ryegrass */
    LAWN_BUFFALO,              /**< Buffalo grass */
    LAWN_CENTIPEDE,            /**< Centipede grass */
    LAWN_BAHIA,                /**< Bahia grass */
    LAWN_GROUND_COVER,         /**< Clover, moss, other ground cover */
    LAWN_TALL_FESCUE,          /**< Tall fescue specifically */
    LAWN_FINE_FESCUE,          /**< Fine fescue varieties */
    LAWN_ANNUAL_RYEGRASS,      /**< Annual ryegrass */
    LAWN_PERENNIAL_RYEGRASS,   /**< Perennial ryegrass */
    LAWN_DICHONDRA,            /**< Dichondra ground cover */
    LAWN_CLOVER_WHITE,         /**< White clover lawn */
    LAWN_CLOVER_MICRO,         /**< Micro clover */
    LAWN_SEDUM_VARIETIES,      /**< Sedum ground cover */
    LAWN_THYME_CREEPING,       /**< Creeping thyme */
    LAWN_MOSS_LAWN,            /**< Moss lawn areas */
    LAWN_ARTIFICIAL,           /**< Artificial turf maintenance */
    LAWN_WILDFLOWER_MIX,       /**< Wildflower meadow */
    LAWN_PRAIRIE_GRASS,        /**< Native prairie grasses */
    LAWN_ORNAMENTAL_GRASS,     /**< Ornamental grass plantings */
    LAWN_SEDGE,                /**< Sedge varieties */
    LAWN_RUSH,                 /**< Rush varieties */
    LAWN_MONDO_GRASS,          /**< Ophiopogon japonicus */
    LAWN_LIRIOPE,              /**< Lily turf */
    LAWN_AJUGA,                /**< Carpet bugleweed */
    LAWN_PACHYSANDRA,          /**< Japanese spurge */
    LAWN_VINCA_MINOR,          /**< Periwinkle ground cover */
    LAWN_ENGLISH_IVY,          /**< Hedera helix */
    LAWN_BISHOP_WEED,          /**< Aegopodium podagraria */
    LAWN_SWEET_WOODRUFF,       /**< Galium odoratum */
    LAWN_WILD_GINGER,          /**< Asarum canadense */
    LAWN_HOSTAS,               /**< Hosta ground cover */
    LAWN_FERNS,                /**< Fern ground cover */
    LAWN_CORAL_BELLS,          /**< Heuchera varieties */
    LAWN_LAMIUM,               /**< Dead nettle */
    LAWN_MAZUS,                /**< Mazus reptans */
    LAWN_VERONICA,             /**< Speedwell ground cover */
    LAWN_PHLOX_SUBULATA,       /**< Creeping phlox */
    LAWN_ICE_PLANT,            /**< Delosperma varieties */
    LAWN_STONECROP,            /**< Sedum ground covers */
    LAWN_HEN_AND_CHICKS,       /**< Sempervivum carpet */
    LAWN_NATIVE_WILDFLOWERS,   /**< Regional wildflower mix */
    LAWN_XEROPHYTIC_MIX,       /**< Drought-tolerant mix */
    LAWN_SHADE_MIX,            /**< Shade-tolerant grass mix */
    LAWN_OTHER                 /**< Other grass types */
} lawn_type_t;

/**
 * @brief Specific succulent and cactus types
 */
typedef enum {
    SUCCULENT_ALOE,            /**< Aloe vera and varieties */
    SUCCULENT_ECHEVERIA,       /**< Echeveria rosettes */
    SUCCULENT_SEDUM,           /**< Sedum varieties */
    SUCCULENT_JADE,            /**< Jade plants */
    SUCCULENT_AGAVE,           /**< Agave and century plants */
    SUCCULENT_BARREL_CACTUS,   /**< Barrel cacti */
    SUCCULENT_PRICKLY_PEAR,    /**< Prickly pear cactus */
    SUCCULENT_CHRISTMAS_CACTUS,/**< Holiday cacti */
    SUCCULENT_SNAKE_PLANT,     /**< Sansevieria */
    SUCCULENT_HAWORTHIA,       /**< Haworthia species */
    SUCCULENT_KALANCHOE,       /**< Kalanchoe varieties */
    SUCCULENT_HENS_AND_CHICKS, /**< Sempervivum */
    SUCCULENT_BURROS_TAIL,     /**< Sedum morganianum */
    SUCCULENT_STRING_OF_PEARLS, /**< Senecio rowleyanus */
    SUCCULENT_ZEBRA_PLANT,     /**< Haworthia zebra */
    SUCCULENT_LITHOPS,         /**< Living stones */
    SUCCULENT_ADENIUM,         /**< Desert rose */
    SUCCULENT_CRASSULA,        /**< Crassula varieties */
    SUCCULENT_AEONIUM,         /**< Aeonium rosettes */
    SUCCULENT_GASTERIA,        /**< Gasteria species */
    SUCCULENT_PORTULACARIA,    /**< Elephant bush */
    SUCCULENT_SENECIO,         /**< Senecio succulents */
    SUCCULENT_EUPHORBIA,       /**< Succulent euphorbias */
    SUCCULENT_PACHYPHYTUM,     /**< Moonstones */
    SUCCULENT_GRAPTOVERIA,     /**< Graptoveria hybrids */
    SUCCULENT_PENCIL_CACTUS,   /**< Euphorbia tirucalli */
    SUCCULENT_CROWN_OF_THORNS, /**< Euphorbia milii */
    SUCCULENT_BUNNY_EARS,      /**< Opuntia microdasys */
    SUCCULENT_GOLDEN_BARREL,   /**< Echinocactus grusonii */
    SUCCULENT_FISHHOOK_CACTUS, /**< Ferocactus species */
    SUCCULENT_THANKSGIVING_CACTUS, /**< Schlumbergera truncata */
    SUCCULENT_EASTER_CACTUS,   /**< Schlumbergera gaertneri */
    SUCCULENT_MOON_CACTUS,     /**< Gymnocalycium mihanovichii */
    SUCCULENT_PANDA_PLANT,     /**< Kalanchoe tomentosa */
    SUCCULENT_MOTHER_IN_LAW_TONGUE, /**< Sansevieria varieties */
    SUCCULENT_ZZ_PLANT,        /**< Zamioculcas zamiifolia */
    SUCCULENT_PONYTAIL_PALM,   /**< Beaucarnea recurvata */
    SUCCULENT_DESERT_WILLOW,   /**< Chilopsis linearis */
    SUCCULENT_OCOTILLO,        /**< Fouquieria splendens */
    SUCCULENT_YUCCA,           /**< Yucca varieties */
    SUCCULENT_CENTURY_PLANT,   /**< Agave americana */
    SUCCULENT_BLUE_AGAVE,      /**< Agave tequilana */
    SUCCULENT_FIRESTICK,       /**< Euphorbia tirucalli */
    SUCCULENT_CANDELABRA,      /**< Euphorbia ingens */
    SUCCULENT_MEDUSA_HEAD,     /**< Euphorbia caput-medusae */
    SUCCULENT_BASEBALL_PLANT,  /**< Euphorbia obesa */
    SUCCULENT_AFRICAN_MILK_TREE, /**< Euphorbia trigona */
    SUCCULENT_BOTTLE_TREE,     /**< Brachychiton rupestris */
    SUCCULENT_MONEY_TREE,      /**< Pachira aquatica */
    SUCCULENT_RUBBER_PLANT,    /**< Ficus elastica */
    SUCCULENT_FIDDLE_LEAF_FIG, /**< Ficus lyrata */
    SUCCULENT_GHOST_PLANT,     /**< Graptopetalum paraguayense */
    SUCCULENT_HOYA,            /**< Wax plant varieties */
    SUCCULENT_STRING_OF_HEARTS, /**< Ceropegia woodii */
    SUCCULENT_STRING_OF_BANANAS, /**< Senecio radicans */
    SUCCULENT_STRING_OF_DOLPHINS, /**< Senecio peregrinus */
    SUCCULENT_PINK_MOONSTONE,  /**< Pachyphytum oviferum */
    SUCCULENT_BLUE_CHALK_STICKS, /**< Senecio serpens */
    SUCCULENT_JELLY_BEAN_PLANT, /**< Sedum rubrotinctum */
    SUCCULENT_DONKEY_TAIL,     /**< Sedum morganianum */
    SUCCULENT_WATCH_CHAIN,     /**< Crassula lycopodioides */
    SUCCULENT_PROPELLER_PLANT, /**< Crassula falcata */
    SUCCULENT_SILVER_DOLLAR,   /**< Crassula arborescens */
    SUCCULENT_OTHER            /**< Other succulents */
} succulent_type_t;

/**
 * @brief Custom plant configuration for PLANT_TYPE_OTHER
 */
typedef struct {
    char custom_name[32];       /**< CUSTOM PLANT NAME: Specific plant species (e.g., "Hibiscus rosa-sinensis") - 32 bytes */
    float water_need_factor;    /**< Water need multiplier (0.1-5.0, default 1.0) */
    uint8_t irrigation_freq;    /**< Recommended irrigation frequency (days between watering) */
    bool prefer_area_based;     /**< True if plant prefers m² measurement, false for plant count */
} custom_plant_config_t;

/**
 * @brief Type of soil in the growing area
 */
typedef enum {
    SOIL_TYPE_CLAY,            /**< Clay soil - retains water well */
    SOIL_TYPE_SANDY,           /**< Sandy soil - drains quickly */
    SOIL_TYPE_LOAMY,           /**< Loamy soil - balanced drainage */
    SOIL_TYPE_PEAT,            /**< Peat soil - organic, moisture retentive */
    SOIL_TYPE_CHALK,           /**< Chalk soil - alkaline, free draining */
    SOIL_TYPE_SILT,            /**< Silt soil - fine particles, retains moisture */
    SOIL_TYPE_POTTING_MIX,     /**< Commercial potting mix */
    SOIL_TYPE_HYDROPONIC       /**< Hydroponic growing medium */
} soil_type_t;

/**
 * @brief Irrigation method used for the channel
 */
typedef enum {
    IRRIGATION_DRIP,           /**< Drip irrigation system */
    IRRIGATION_SPRINKLER,      /**< Sprinkler or spray irrigation */
    IRRIGATION_SOAKER_HOSE,    /**< Soaker hose irrigation */
    IRRIGATION_MICRO_SPRAY,    /**< Micro spray heads */
    IRRIGATION_FLOOD,          /**< Flood irrigation */
    IRRIGATION_SUBSURFACE      /**< Subsurface irrigation */
} irrigation_method_t;

/**
 * @brief Channel coverage information - either area or plant count
 */
typedef struct {
    bool use_area;             /**< True for area-based, false for plant count */
    union {
        struct {
            float area_m2;     /**< Area in square meters */
        } area;
        struct {
            uint16_t count;    /**< Number of individual plants */
        } plants;
    };
} channel_coverage_t;

/**
 * @brief Specific plant information based on the main plant type
 */
typedef struct {
    plant_type_t main_type;    /**< Main plant category */
    union {
        vegetable_type_t vegetable;    /**< Specific vegetable type */
        herb_type_t herb;              /**< Specific herb type */
        flower_type_t flower;          /**< Specific flower type */
        shrub_type_t shrub;            /**< Specific shrub type */
        tree_type_t tree;              /**< Specific tree type */
        lawn_type_t lawn;              /**< Specific lawn type */
        succulent_type_t succulent;    /**< Specific succulent type */
        custom_plant_config_t custom;  /**< Custom plant configuration */
    } specific;
} plant_info_t;

/**
 * @brief Master valve configuration for intelligent timing control
 */
typedef struct {
    bool enabled;                    /**< Whether master valve functionality is enabled */
    int16_t pre_start_delay_sec;     /**< Seconds to open master valve BEFORE zone valve (negative = after) */
    int16_t post_stop_delay_sec;     /**< Seconds to keep master valve open AFTER zone valve closes (negative = close before) */
    uint8_t overlap_grace_sec;       /**< Grace period (seconds) to keep master open between consecutive tasks */
    bool auto_management;            /**< Enable automatic master valve management for all tasks */
    struct gpio_dt_spec valve;       /**< GPIO specification for the master valve control */
    bool is_active;                  /**< Current state of master valve */
} master_valve_config_t;

/**
 * @brief Complete definition of a watering event including scheduling and quantity
 */
typedef struct watering_event_t {
    schedule_type_t schedule_type;  /**< Type of schedule (daily or periodic) */
    watering_mode_t watering_mode;  /**< Mode of watering (duration or volume) */

    /** Schedule-specific parameters */
    union {
        struct {
            uint8_t days_of_week;   /**< Bitmask of days (bit 0=Sunday, 1=Monday, etc.) */
        } daily;

        struct {
            uint8_t interval_days;  /**< Number of days between watering events */
        } periodic;
    } schedule;

    /** Watering quantity-specific parameters */
    union {
        struct {
            uint8_t duration_minutes;  /**< Duration in minutes for time-based watering */
        } by_duration;

        struct {
            uint16_t volume_liters;    /**< Volume in liters for volume-based watering */
        } by_volume;
    } watering;

    /** Time to start watering event */
    struct {
        uint8_t hour;    /**< Hour of day (0-23) */
        uint8_t minute;  /**< Minute of hour (0-59) */
    } start_time;

    bool auto_enabled;  /**< Whether this event is enabled for automatic scheduling */
} watering_event_t;

/**
 * @brief Definition of a watering channel including its configuration and hardware
 */
typedef struct {
    watering_event_t watering_event;  /**< Configuration for automatic scheduling */
    uint32_t last_watering_time;      /**< Timestamp of last watering event */
    char name[64];                    /**< CHANNEL NAME: User-friendly name for the channel (e.g., "Front Garden") - 64 bytes */
    struct gpio_dt_spec valve;        /**< GPIO specification for the valve control */
    bool is_active;                   /**< Whether this channel is currently active */
    
    /* Enhanced growing environment configuration */
    uint16_t plant_db_index;           /**< Index into plant_full_database (0-based, UINT16_MAX = not set) */
    uint8_t soil_db_index;             /**< Index into soil_enhanced_database (0-based, UINT8_MAX = not set) */
    uint8_t irrigation_method_index;   /**< Index into irrigation_methods_database (0-based, UINT8_MAX = not set) */
    
    /* Coverage specification */
    bool use_area_based;               /**< True = area-based calculation, false = plant count-based */
    union {
        float area_m2;                 /**< Area in square meters (for area-based) */
        uint16_t plant_count;          /**< Number of plants (for plant-count-based) */
    } coverage;
    
    /* Automatic mode settings */
    watering_mode_t auto_mode;         /**< WATERING_AUTOMATIC_QUALITY or WATERING_AUTOMATIC_ECO */
    float max_volume_limit_l;          /**< Maximum irrigation volume limit (liters) */
    bool enable_cycle_soak;            /**< Enable cycle and soak for clay soils */
    
    /* Plant lifecycle tracking */
    uint32_t planting_date_unix;       /**< When plants were established (Unix timestamp) */
    uint16_t days_after_planting;      /**< Calculated field - days since planting */
    
    /* Environmental overrides */
    float latitude_deg;                /**< Location latitude for solar calculations */
    uint8_t sun_exposure_pct;          /**< Site-specific sun exposure (0-100%) */
    
    /* Water balance state (runtime) */
    struct water_balance_t *water_balance; /**< Current water balance state */
    uint32_t last_calculation_time;    /**< Last automatic calculation timestamp */
    
    /* Legacy fields for backward compatibility */
    plant_info_t plant_info;          /**< Detailed plant type and specific variety (legacy) */
    plant_type_t plant_type;          /**< Main type of plant being grown (legacy) */
    soil_type_t soil_type;            /**< Type of soil in the growing area (legacy) */
    irrigation_method_t irrigation_method; /**< Method of irrigation used (legacy) */
    channel_coverage_t coverage_legacy; /**< Area or plant count information (legacy) */
    uint8_t sun_percentage;           /**< Percentage of direct sunlight (legacy) */
    custom_plant_config_t custom_plant; /**< Custom plant settings (legacy) */
    
    /* Extended fields for BLE service compatibility */
    struct {
        bool enabled;                 /**< Rain compensation enabled */
        float sensitivity;            /**< Rain sensitivity factor */
        uint16_t lookback_hours;      /**< Hours to look back for rain */
        float skip_threshold_mm;      /**< Rain threshold to skip watering */
        float reduction_factor;       /**< Reduction factor for rain */
    } rain_compensation;
    
    struct {
        bool enabled;                 /**< Temperature compensation enabled */
        float base_temperature;       /**< Base temperature for calculations */
        float sensitivity;            /**< Temperature sensitivity factor */
        float min_factor;             /**< Minimum compensation factor */
        float max_factor;             /**< Maximum compensation factor */
    } temp_compensation;
    
    struct {
        float reduction_percentage;   /**< Last rain reduction percentage */
        bool skip_watering;           /**< Whether rain caused skip */
    } last_rain_compensation;
    
    struct {
        float compensation_factor;    /**< Last temperature compensation factor */
        float adjusted_requirement;   /**< Last adjusted requirement */
    } last_temp_compensation;
    
    struct {
        bool configured;              /**< Whether interval mode is configured */
        uint16_t watering_minutes;    /**< Watering duration minutes */
        uint8_t watering_seconds;     /**< Watering duration seconds */
        uint16_t pause_minutes;       /**< Pause duration minutes */
        uint8_t pause_seconds;        /**< Pause duration seconds */
        uint64_t phase_start_time;    /**< When current phase started */
    } interval_config;

    // Shadow copy compatible with interval_config_t APIs
    struct {
        uint16_t watering_minutes;
        uint8_t watering_seconds;
        uint16_t pause_minutes;
        uint8_t pause_seconds;
        uint32_t total_target;
        uint32_t cycles_completed;
        bool currently_watering;
        uint32_t phase_start_time;
        uint32_t phase_remaining_sec;
        bool configured;
    } interval_config_shadow;
    
    struct {
        bool use_custom_soil;         /**< Whether to use custom soil */
        struct {
            char name[32];            /**< Custom soil name */
            float field_capacity;     /**< Field capacity percentage */
            float wilting_point;      /**< Wilting point percentage */
            float infiltration_rate;  /**< Infiltration rate mm/hr */
            float bulk_density;       /**< Bulk density g/cm³ */
            float organic_matter;     /**< Organic matter percentage */
        } custom;
    } soil_config;
    
    struct {
        bool basic_configured;        /**< Basic configuration complete */
        bool growing_env_configured;  /**< Growing environment complete */
        bool compensation_configured; /**< Compensation settings complete */
        bool custom_soil_configured;  /**< Custom soil complete */
        bool interval_configured;    /**< Interval settings complete */
        uint8_t configuration_score; /**< Configuration score 0-100 */
        uint32_t last_reset_timestamp; /**< Last reset timestamp */
    } config_status;
} watering_channel_t;

/**
 * @brief A watering task represents a single watering operation to be executed
 */
typedef struct {
    watering_channel_t *channel;  /**< Channel to be watered */
    watering_trigger_type_t trigger_type; /**< How this task was triggered */

    /** Task-specific parameters depending on watering mode */
    union {
        struct {
            uint32_t start_time;  /**< Start time for duration-based watering */
        } by_time;

        struct {
            uint32_t volume_liters;  /**< Target volume for volume-based watering */
        } by_volume;
    };
} watering_task_t;

/**
 * @brief System status codes for the watering system
 */
typedef enum {
    WATERING_STATUS_OK = 0,            /**< System operating normally */
    WATERING_STATUS_NO_FLOW = 1,       /**< No flow detected when valve is open */
    WATERING_STATUS_UNEXPECTED_FLOW = 2,  /**< Flow detected when all valves closed */
    WATERING_STATUS_FAULT = 3,         /**< System in fault state requiring manual reset */
    WATERING_STATUS_RTC_ERROR = 4,     /**< RTC failure detected */
    WATERING_STATUS_LOW_POWER = 5      /**< System in low power mode */
} watering_status_t;

/**
 * @brief Power mode configuration for the system
 */
typedef enum {
    POWER_MODE_NORMAL,          /**< Normal operation mode */
    POWER_MODE_ENERGY_SAVING,   /**< Energy-saving mode with reduced polling */
    POWER_MODE_ULTRA_LOW_POWER  /**< Ultra-low power mode with minimal activity */
} power_mode_t;

/**
 * @brief Initialize the watering system
 * 
 * Sets up all channels, GPIO pins, and internal state.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_init(void);

/**
 * @brief Start background tasks for watering operations
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_start_tasks(void);

/**
 * @brief Stop all background watering tasks
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_stop_tasks(void);

/**
 * @brief Get a pointer to the specified watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param channel Pointer to store the channel reference
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_channel(uint8_t channel_id, watering_channel_t **channel);

/**
 * @brief Turn on a specific watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_channel_on(uint8_t channel_id);

/**
 * @brief Turn off a specific watering channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_channel_off(uint8_t channel_id);

/**
 * @brief Add a watering task to the execution queue
 * 
 * @param task Pointer to task definition
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_task(watering_task_t *task);

/**
 * @brief Process the next task in the queue
 * 
 * @return 1 if a task was processed, 0 if no tasks available, negative error code on failure
 */
int watering_process_next_task(void);

/**
 * @brief Execute the scheduler to check for automatic watering events
 * 
 * Checks all channels for scheduled events that should run at current time.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_scheduler_run(void);

/**
 * @brief Check active tasks for completion or issues
 * 
 * @return 1 if tasks are active, 0 if idle, negative error code on failure
 */
int watering_check_tasks(void);

/**
 * @brief Clean up completed tasks and release resources
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_cleanup_tasks(void);

/**
 * @brief Set the flow sensor calibration value
 * 
 * @param pulses_per_liter Number of sensor pulses per liter of water
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_flow_calibration(uint32_t pulses_per_liter);

/**
 * @brief Get the current flow sensor calibration value
 * 
 * @param pulses_per_liter Pointer to store the calibration value
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_flow_calibration(uint32_t *pulses_per_liter);

/**
 * @brief Save system configuration to persistent storage
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_save_config(void);

/**
 * @brief Saves flow sensor calibration and all channel configurations with priority handling
 * 
 * @param is_priority If true, uses shorter throttle time for critical saves (e.g., BLE config changes)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_save_config_priority(bool is_priority);

/**
 * @brief Load system configuration from persistent storage
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_load_config(void);

/**
 * @brief Validate watering event configuration
 * 
 * @param event Pointer to watering event to validate
 * @return WATERING_SUCCESS if valid, error code if invalid
 */
watering_error_t watering_validate_event_config(const watering_event_t *event);

/**
 * @brief Get the current system status
 * 
 * @param status Pointer to store the status
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_status(watering_status_t *status);

/**
 * @brief Get the current system state
 * 
 * @param state Pointer to store the state
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_state(watering_state_t *state);

/**
 * @brief Reset the system from a fault state
 * 
 * @return WATERING_SUCCESS if reset successful, error code on failure
 */
watering_error_t watering_reset_fault(void);

/**
 * @brief Set the system power mode
 * 
 * @param mode New power mode to set
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_power_mode(power_mode_t mode);

/**
 * @brief Get the current power mode
 * 
 * @param mode Pointer to store the power mode
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_power_mode(power_mode_t *mode);

/* ===== MASTER VALVE FUNCTIONS ===== */

/**
 * @brief Set master valve configuration
 * 
 * @param config Pointer to master valve configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_set_config(const master_valve_config_t *config);

/**
 * @brief Get master valve configuration
 * 
 * @param config Pointer to store master valve configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_get_config(master_valve_config_t *config);

/**
 * @brief Notify master valve system about upcoming task
 * 
 * This allows the master valve logic to prepare for overlapping tasks
 * 
 * @param start_time When the next task will start (k_uptime_get_32() format)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_notify_upcoming_task(uint32_t start_time);

/**
 * @brief Clear pending task notification
 * 
 * Called when a scheduled task is cancelled or completed
 */
void master_valve_clear_pending_task(void);

/**
 * @brief Manually open master valve (for BLE control)
 * 
 * This function allows manual control of the master valve via BLE.
 * Only works when auto_management is disabled.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_manual_open(void);

/**
 * @brief Manually close master valve (for BLE control)
 * 
 * This function allows manual control of the master valve via BLE.
 * Only works when auto_management is disabled.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t master_valve_manual_close(void);

/**
 * @brief Get current master valve state
 * 
 * @return true if master valve is open, false if closed
 */
bool master_valve_is_open(void);

/* ===== CHANNEL TASK FUNCTIONS ===== */

/**
 * @brief Add a duration-based watering task for a specific channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param minutes Watering duration in minutes
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_duration_task(uint8_t channel_id, uint16_t minutes);

/**
 * @brief Add a volume-based watering task for a specific channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param liters Water volume in liters
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_add_volume_task(uint8_t channel_id, uint16_t liters);

/**
 * @brief Cancel all tasks and clear the task queue
 * 
 * @return Number of tasks canceled
 */
int watering_cancel_all_tasks(void);

/**
 * @brief Get the status of the task queue
 * 
 * @param pending_count Pointer where the number of pending tasks will be stored
 * @param active Flag indicating if there is an active task
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_queue_status(uint8_t *pending_count, bool *active);

/**
 * @brief Clear all run-time error flags and counters
 *
 * Useful for a manual "reset errors" command from BLE / console.
 *
 * @return WATERING_SUCCESS
 */
watering_error_t watering_clear_errors(void);

/*
 * Specific plant type management functions
 */

/**
 * @brief Set specific vegetable type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param vegetable_type Specific vegetable variety
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_vegetable_type(uint8_t channel_id, vegetable_type_t vegetable_type);

/**
 * @brief Get specific vegetable type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param vegetable_type Pointer to store the vegetable type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_vegetable_type(uint8_t channel_id, vegetable_type_t *vegetable_type);

/**
 * @brief Set specific herb type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param herb_type Specific herb variety
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_herb_type(uint8_t channel_id, herb_type_t herb_type);

/**
 * @brief Get specific herb type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param herb_type Pointer to store the herb type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_herb_type(uint8_t channel_id, herb_type_t *herb_type);

/**
 * @brief Set specific flower type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param flower_type Specific flower variety
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_flower_type(uint8_t channel_id, flower_type_t flower_type);

/**
 * @brief Get specific flower type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param flower_type Pointer to store the flower type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_flower_type(uint8_t channel_id, flower_type_t *flower_type);

/**
 * @brief Set specific tree type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param tree_type Specific tree variety
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_tree_type(uint8_t channel_id, tree_type_t tree_type);

/**
 * @brief Get specific tree type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param tree_type Pointer to store the tree type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_tree_type(uint8_t channel_id, tree_type_t *tree_type);

/**
 * @brief Set specific lawn type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param lawn_type Specific grass variety
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_lawn_type(uint8_t channel_id, lawn_type_t lawn_type);

/**
 * @brief Get specific lawn type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param lawn_type Pointer to store the lawn type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_lawn_type(uint8_t channel_id, lawn_type_t *lawn_type);

/**
 * @brief Set specific succulent type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param succulent_type Specific succulent variety
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_succulent_type(uint8_t channel_id, succulent_type_t succulent_type);

/**
 * @brief Get specific succulent type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param succulent_type Pointer to store the succulent type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_succulent_type(uint8_t channel_id, succulent_type_t *succulent_type);

/**
 * @brief Set specific shrub type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param shrub_type Specific shrub variety
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_shrub_type(uint8_t channel_id, shrub_type_t shrub_type);

/**
 * @brief Get specific shrub type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param shrub_type Pointer to store the shrub type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_shrub_type(uint8_t channel_id, shrub_type_t *shrub_type);

/**
 * @brief Get complete plant information for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param plant_info Pointer to store the complete plant information
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_plant_info(uint8_t channel_id, plant_info_t *plant_info);

/**
 * @brief Set complete plant information for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param plant_info Complete plant information to set
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_plant_info(uint8_t channel_id, const plant_info_t *plant_info);

/*
 * Plant and growing environment configuration functions
 */

/**
 * @brief Set the plant type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param plant_type Type of plant being grown
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_plant_type(uint8_t channel_id, plant_type_t plant_type);

/**
 * @brief Get the plant type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param plant_type Pointer to store the plant type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_plant_type(uint8_t channel_id, plant_type_t *plant_type);

/**
 * @brief Set the soil type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param soil_type Type of soil in the growing area
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_soil_type(uint8_t channel_id, soil_type_t soil_type);

/**
 * @brief Get the soil type for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param soil_type Pointer to store the soil type
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_soil_type(uint8_t channel_id, soil_type_t *soil_type);

/**
 * @brief Set the irrigation method for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param irrigation_method Method of irrigation used
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_irrigation_method(uint8_t channel_id, irrigation_method_t irrigation_method);

/**
 * @brief Get the irrigation method for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param irrigation_method Pointer to store the irrigation method
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_irrigation_method(uint8_t channel_id, irrigation_method_t *irrigation_method);

/**
 * @brief Set the coverage area for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param area_m2 Area in square meters
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_coverage_area(uint8_t channel_id, float area_m2);

/**
 * @brief Set the plant count for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param count Number of individual plants
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_plant_count(uint8_t channel_id, uint16_t count);

/**
 * @brief Get the coverage information for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param coverage Pointer to store the coverage information
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_coverage(uint8_t channel_id, channel_coverage_t *coverage);

/**
 * @brief Set the sun percentage for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param sun_percentage Percentage of direct sunlight (0-100%)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_sun_percentage(uint8_t channel_id, uint8_t sun_percentage);

/**
 * @brief Get the sun percentage for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param sun_percentage Pointer to store the sun percentage
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_sun_percentage(uint8_t channel_id, uint8_t *sun_percentage);

/**
 * @brief Set custom plant configuration for a channel (when plant_type == PLANT_TYPE_OTHER)
 * 
 * @param channel_id Channel ID (0-based index)
 * @param custom_config Pointer to custom plant configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_custom_plant(uint8_t channel_id, const custom_plant_config_t *custom_config);

/**
 * @brief Get custom plant configuration for a channel
 * 
 * @param channel_id Channel ID (0-based index)
 * @param custom_config Pointer to store the custom plant configuration
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_custom_plant(uint8_t channel_id, custom_plant_config_t *custom_config);

/**
 * @brief Get the recommended coverage measurement type based on irrigation method
 * 
 * @param irrigation_method The irrigation method to check
 * @return true if area-based (m²) is recommended, false if plant-count is recommended
 */
bool watering_recommend_area_based_measurement(irrigation_method_t irrigation_method);

/**
 * @brief Get water need factor for a specific plant type
 * 
 * @param plant_type Type of plant
 * @param custom_config Custom plant config (used only if plant_type == PLANT_TYPE_OTHER)
 * @return Water need factor (multiplier for base water requirements)
 */
float watering_get_plant_water_factor(plant_type_t plant_type, const custom_plant_config_t *custom_config);

/**
 * @brief Validate if coverage measurement type matches irrigation method recommendation
 * 
 * @param irrigation_method The irrigation method
 * @param use_area_based Current coverage measurement type (true = m², false = plant count)
 * @return true if combination is optimal, false if suboptimal
 */
bool watering_validate_coverage_method_match(irrigation_method_t irrigation_method, bool use_area_based);

/**
 * @brief Get comprehensive channel environment information
 * 
 * @param channel_id Channel ID (0-based index)
 * @param plant_type Pointer to store plant type (can be NULL)
 * @param soil_type Pointer to store soil type (can be NULL)
 * @param irrigation_method Pointer to store irrigation method (can be NULL)
 * @param coverage Pointer to store coverage information (can be NULL)
 * @param sun_percentage Pointer to store sun percentage (can be NULL)
 * @param custom_config Pointer to store custom plant configuration (can be NULL)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_channel_environment(uint8_t channel_id, 
                                                 plant_type_t *plant_type,
                                                 soil_type_t *soil_type,
                                                 irrigation_method_t *irrigation_method,
                                                 channel_coverage_t *coverage,
                                                 uint8_t *sun_percentage,
                                                 custom_plant_config_t *custom_config);

/**
 * @brief Set comprehensive channel environment configuration
 * 
 * @param channel_id Channel ID (0-based index)
 * @param plant_type Type of plant being grown
 * @param soil_type Type of soil in the growing area
 * @param irrigation_method Method of irrigation used
 * @param coverage Pointer to coverage information (area or plant count)
 * @param sun_percentage Percentage of direct sunlight (0-100%)
 * @param custom_config Pointer to custom plant configuration (required if plant_type == PLANT_TYPE_OTHER)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_channel_environment(uint8_t channel_id,
                                                 plant_type_t plant_type,
                                                 soil_type_t soil_type,
                                                 irrigation_method_t irrigation_method,
                                                 const channel_coverage_t *coverage,
                                                 uint8_t sun_percentage,
                                                 const custom_plant_config_t *custom_config);

/**
 * @brief Get the number of pending tasks in the queue
 * 
 * @return Number of pending tasks
 */
int watering_get_pending_tasks_count(void);

/**
 * @brief Get the number of completed tasks
 * 
 * @return Number of completed tasks since system start
 */
int watering_get_completed_tasks_count(void);

/**
 * @brief Increment the completed tasks counter
 * Internal function called when a task completes
 */
void watering_increment_completed_tasks_count(void);

/**
 * @brief Clear all tasks from the task queue
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
int watering_clear_task_queue(void);

/**
 * @brief Stop the currently running task
 * 
 * @return true if a task was stopped, false if no task was running
 */
bool watering_stop_current_task(void);

/**
 * @brief Pause the currently running task
 * 
 * @return true if a task was paused, false if no task was running or task cannot be paused
 */
bool watering_pause_current_task(void);

/**
 * @brief Resume the currently paused task
 * 
 * @return true if a task was resumed, false if no task was paused or task cannot be resumed
 */
bool watering_resume_current_task(void);

/**
 * @brief Check if the current task is paused
 * 
 * @return true if current task is paused, false otherwise
 */
bool watering_is_current_task_paused(void);

/**
 * @brief Get task queue status
 * 
 * @param pending_count Pointer to store the number of pending tasks
 * @param active Pointer to store whether a task is currently active
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_queue_status(uint8_t *pending_count, bool *active);

/**
 * @brief Cancel all watering tasks
 * 
 * @return 0 on success, negative error code on failure
 */
int watering_cancel_all_tasks(void);

/**
 * @brief Get the current running task
 * 
 * @return Pointer to current task or NULL if no task running
 */
watering_task_t *watering_get_current_task(void);

/**
 * @brief Get channel statistics for BLE API
 * 
 * @param channel_id Channel ID (0-7)
 * @param total_volume_ml Pointer to store total volume in ml
 * @param last_volume_ml Pointer to store last watering volume in ml
 * @param watering_count Pointer to store total watering count
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_channel_statistics(uint8_t channel_id,
                                                uint32_t *total_volume_ml,
                                                uint32_t *last_volume_ml,
                                                uint32_t *watering_count);

/**
 * @brief Update channel statistics after watering event
 * 
 * @param channel_id Channel ID (0-7)
 * @param volume_ml Volume watered in ml
 * @param timestamp Timestamp of watering event
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_update_channel_statistics(uint8_t channel_id,
                                                   uint32_t volume_ml,
                                                   uint32_t timestamp);

/**
 * @brief Reset channel statistics
 * 
 * @param channel_id Channel ID (0-7)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_reset_channel_statistics(uint8_t channel_id);

/**
 * @brief Set the planting date for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param planting_date_unix Unix timestamp of when plants were established
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_planting_date(uint8_t channel_id, uint32_t planting_date_unix);

/**
 * @brief Get the planting date for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param planting_date_unix Pointer to store the planting date Unix timestamp
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_planting_date(uint8_t channel_id, uint32_t *planting_date_unix);

/**
 * @brief Update days after planting for a channel
 * 
 * This function calculates the current days after planting based on the 
 * planting date and current time. Should be called periodically or when
 * planting date is updated.
 * 
 * @param channel_id Channel ID (0-7)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_update_days_after_planting(uint8_t channel_id);

/**
 * @brief Get days after planting for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param days_after_planting Pointer to store the days after planting
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_days_after_planting(uint8_t channel_id, uint16_t *days_after_planting);

/**
 * @brief Update days after planting for all channels
 * 
 * This function should be called periodically (e.g., daily) to keep the
 * days after planting calculations current for all channels.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_update_all_days_after_planting(void);

/**
 * @brief Set the latitude for a channel (for solar radiation calculations)
 * 
 * @param channel_id Channel ID (0-7)
 * @param latitude_deg Latitude in degrees (-90 to +90)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_latitude(uint8_t channel_id, float latitude_deg);

/**
 * @brief Get the latitude for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param latitude_deg Pointer to store the latitude in degrees
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_latitude(uint8_t channel_id, float *latitude_deg);

/**
 * @brief Set the sun exposure percentage for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param sun_exposure_pct Site-specific sun exposure percentage (0-100%)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_sun_exposure(uint8_t channel_id, uint8_t sun_exposure_pct);

/**
 * @brief Get the sun exposure percentage for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param sun_exposure_pct Pointer to store the sun exposure percentage
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_sun_exposure(uint8_t channel_id, uint8_t *sun_exposure_pct);

/**
 * @brief Set comprehensive environmental configuration for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param latitude_deg Latitude in degrees (-90 to +90)
 * @param sun_exposure_pct Site-specific sun exposure percentage (0-100%)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_environmental_config(uint8_t channel_id, 
                                                  float latitude_deg, 
                                                  uint8_t sun_exposure_pct);

/**
 * @brief Get comprehensive environmental configuration for a channel
 * 
 * @param channel_id Channel ID (0-7)
 * @param latitude_deg Pointer to store latitude in degrees (can be NULL)
 * @param sun_exposure_pct Pointer to store sun exposure percentage (can be NULL)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_environmental_config(uint8_t channel_id, 
                                                  float *latitude_deg, 
                                                  uint8_t *sun_exposure_pct);

/**
 * @brief Run automatic irrigation calculations for all channels
 * 
 * This function processes all channels configured for automatic irrigation
 * and schedules irrigation tasks based on FAO-56 calculations.
 * 
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_run_automatic_calculations(void);

/**
 * @brief Set automatic calculation interval
 * 
 * @param interval_hours Interval between automatic calculations in hours (1-24)
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_auto_calc_interval(uint8_t interval_hours);

/**
 * @brief Enable or disable automatic calculations
 * 
 * @param enabled True to enable, false to disable
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_auto_calc_enabled(bool enabled);

/**
 * @brief Get automatic calculation status
 * 
 * @param enabled Pointer to store enabled status
 * @param interval_hours Pointer to store interval in hours
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_auto_calc_status(bool *enabled, uint8_t *interval_hours);

/**
 * @brief Get the friendly name of a channel
 *
 * @param channel_id Channel ID (0-7)
 * @param out_name Buffer to receive the name (null-terminated)
 * @param out_name_size Size of the output buffer
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_channel_name(uint8_t channel_id, char *out_name, size_t out_name_size);

/**
 * @brief Set the friendly name of a channel
 *
 * @param channel_id Channel ID (0-7)
 * @param name Null-terminated name to set
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_set_channel_name(uint8_t channel_id, const char *name);

/* Forward declarations for FAO-56 types - removed to avoid conflicts with generated files */

/**
 * @brief Save complete channel configuration to NVS
 * 
 * @param ch Channel ID
 * @param channel Channel structure with all configuration data
 * @return 0 on success, negative error code on failure
 */
int nvs_save_complete_channel_config(uint8_t ch, const watering_channel_t *channel);

/**
 * @brief Load complete channel configuration from NVS
 * 
 * @param ch Channel ID
 * @param channel Channel structure to fill with configuration data
 * @return 0 on success, negative error code on failure
 */
int nvs_load_complete_channel_config(uint8_t ch, watering_channel_t *channel);

/**
 * @brief Rain integration status structure for monitoring
 */
typedef struct {
    bool sensor_active;                    /**< Rain sensor is active and responding */
    bool integration_enabled;              /**< Rain integration is enabled */
    uint32_t last_pulse_time;             /**< Last rain pulse timestamp */
    float calibration_mm_per_pulse;        /**< Current calibration setting */
    float rainfall_last_hour;             /**< Rainfall in current hour (mm) */
    float rainfall_last_24h;              /**< Rainfall in last 24 hours (mm) */
    float rainfall_last_48h;              /**< Rainfall in last 48 hours (mm) */
    float sensitivity_pct;                 /**< Rain sensitivity percentage */
    float skip_threshold_mm;              /**< Skip irrigation threshold (mm) */
    float channel_reduction_pct[WATERING_CHANNELS_COUNT]; /**< Reduction % per channel */
    bool channel_skip_irrigation[WATERING_CHANNELS_COUNT]; /**< Skip irrigation per channel */
    uint16_t hourly_entries;              /**< Number of hourly history entries */
    uint16_t daily_entries;               /**< Number of daily history entries */
    uint32_t storage_usage_bytes;         /**< Storage usage in bytes */
} rain_integration_status_t;

/**
 * @brief Get comprehensive system status including rain sensor information
 * 
 * @param status_buffer Buffer to store status information
 * @param buffer_size Size of the status buffer
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_system_status_detailed(char *status_buffer, uint16_t buffer_size);

/**
 * @brief Get rain sensor integration status for monitoring
 * 
 * @param integration_status Buffer to store integration status
 * @return WATERING_SUCCESS on success, error code on failure
 */
watering_error_t watering_get_rain_integration_status(rain_integration_status_t *integration_status);

#endif // WATERING_H