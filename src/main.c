// =============================================================================
//   SKYWARDEN  -  a side-scrolling planet-defense shooter for Vircon32
//
//   Rules follow the 1982 Atari 2600 side-scrolling planet-defense cartridge as
//   documented in the commented disassembly by Nick Bensema & Manuel Polik
//   (point values, enemy slot table, lives/bomb counts, starting waves, planet
//   restoration). Code, graphics, sounds and characters are original.
// =============================================================================

#include "video.h"
#include "input.h"
#include "audio.h"
#include "time.h"
#include "math.h"
#include "string.h"
#include "misc.h"
#include "regions.h"

// -----------------------------------------------------------------------------
//   SOUNDS AND CHANNELS
// -----------------------------------------------------------------------------

#define SND_LASER       0
#define SND_BOOM        1
#define SND_BIG_BOOM    2
#define SND_SMART_BOMB  3
#define SND_HYPERSPACE  4
#define SND_ABDUCT      5
#define SND_MUTATE      6
#define SND_CATCH       7
#define SND_EXTRA_LIFE  8
#define SND_WAVE_CLEAR  9
#define SND_BLIP        10
#define SND_ENEMY_SHOT  11
#define SND_MATERIALIZE 12

#define CH_LASER  0
#define CH_BOOM   1
#define CH_BOOM2  2
#define CH_EVENT  3
#define CH_JINGLE 4
#define CH_ESHOT  5
#define CH_SPAWN  6

// -----------------------------------------------------------------------------
//   GEOMETRY
// -----------------------------------------------------------------------------

#define WORLD_W   2560
#define HALF_W    1280
#define PF_T      62       // top of the flying area
#define GROUND_Y  300      // top of the city band
#define CITY_B    352      // bottom of the city band
#define SHIP_W    32
#define SHIP_H    18

// scanner (radar) at the top of the screen
#define SCAN_X    160
#define SCAN_Y    6
#define SCAN_W    320
#define SCAN_H    46

// -----------------------------------------------------------------------------
//   RULE CONSTANTS (from the 2600 cartridge)
// -----------------------------------------------------------------------------

#define T_MINER    0
#define T_HUNTER   1
#define T_HIVE     2
#define T_STINGER  3
#define T_GRABBER  4
#define T_RAVAGER  5

// points per enemy type, cartridge tables LFF00/LFEFA (BCD hundreds/ones)
int[ 6 ] enemy_points = { 250, 200, 1000, 500, 150, 150 };

// enemy slots filled at the start of each wave (cartridge table LFF18);
// slot 7 is a dormant hunter that only wakes up if the wave drags on
int[ 8 ] wave_slots = { T_MINER, T_HIVE, T_HIVE, T_GRABBER, T_GRABBER, T_GRABBER, T_GRABBER, T_HUNTER };

#define START_LIVES       3
#define START_BOMBS       3
#define BONUS_EVERY       10000
#define COLONISTS         6
#define STINGERS_PER_HIVE 3
#define HUNTER_WAKE_TIME  1200     // 20 seconds

// game variations: starting wave (cartridge table LFEE8) and barren planet
// (cartridge table LFECD, where the planet starts with nobody to protect)
#define NUM_GAMES 6
int[ NUM_GAMES ] game_start_wave = { 1, 3, 5, 1, 3, 5 };
int[ NUM_GAMES ] game_barren     = { 0, 0, 0, 1, 1, 1 };

// -----------------------------------------------------------------------------
//   DATA STRUCTURES
// -----------------------------------------------------------------------------

struct enemy
{
    bool alive;
    int type;
    float x, y;            // world position of the top-left corner
    float vx, vy;
    int w, h;
    int spawn;             // frames left materializing (harmless)
    int timer;
    int fire_timer;
    int target;            // colonist index a grabber is after, or -1
    bool carrying;
};

#define C_GROUND  0
#define C_CARRIED 1
#define C_FALLING 2
#define C_HELD    3
#define C_DEAD    4

struct colonist
{
    int state;
    float x, y;
    float vx, vy;
    float fall_from;
};

struct shot
{
    bool alive;
    float x, y, vx, vy;
    int life;
};

struct particle
{
    bool alive;
    float x, y, vx, vy;
    int life;
    int color;
};

struct star
{
    float x;
    int y;
    int color;
};

#define MAX_ENEMIES   40
#define MAX_SHOTS     16
#define MAX_MINES     16
#define MAX_PARTICLES 160
#define NUM_STARS     70
#define NUM_BUILDINGS 64

enemy[ MAX_ENEMIES ] enemies;
colonist[ COLONISTS ] colonists;
shot[ MAX_SHOTS ] eshots;
shot[ MAX_MINES ] mines;
particle[ MAX_PARTICLES ] particles;
star[ NUM_STARS ] stars;
int[ NUM_BUILDINGS ] bld_w;
int[ NUM_BUILDINGS ] bld_h;
int[ NUM_BUILDINGS ] bld_win;

// -----------------------------------------------------------------------------
//   GLOBAL STATE
// -----------------------------------------------------------------------------

#define GS_TITLE    0
#define GS_PLAY     1
#define GS_DEAD     2
#define GS_WAVEEND  3
#define GS_GAMEOVER 4

int game_state;
int game_number;
int score;
int high_score;
int next_bonus;
int lives;
int bombs;
int wave;
int wave_frames;
int state_timer;
bool paused;
bool planet_alive;
int planet_flash;
int screen_flash;
int title_frames;

// player
float px, py, pvx;
int facing;            // 1 right, -1 left
float anchor;          // screen x the ship sits at
float cam;             // world x of the left screen edge
bool thrusting;
bool in_city;
bool bomb_armed;
int hyper_cooldown;
bool hyper_armed;

// laser (one at a time, like the cartridge's single missile)
bool laser_on;
float laser_x;
float laser_y;
int laser_dir;
float laser_len;
int laser_reload;

// -----------------------------------------------------------------------------
//   HELPERS
// -----------------------------------------------------------------------------

int rnd( int n )
{
    int r = rand();
    if( r < 0 ) r = -r;
    return r % n;
}

float frnd( float a, float b )
{
    return a + (b - a) * rnd( 10000 ) / 10000.0;
}

float fsign( float v )
{
    if( v < 0 ) return -1.0;
    if( v > 0 ) return 1.0;
    return 0.0;
}

float fclamp( float v, float lo, float hi )
{
    if( v < lo ) return lo;
    if( v > hi ) return hi;
    return v;
}

float wrap( float x )
{
    while( x < 0 ) x += WORLD_W;
    while( x >= WORLD_W ) x -= WORLD_W;
    return x;
}

// shortest signed distance a - b around the wrapping world
float wdist( float a, float b )
{
    float d = a - b;
    while( d > HALF_W ) d -= WORLD_W;
    while( d < -HALF_W ) d += WORLD_W;
    return d;
}

// world x -> screen x
float sxof( float x )
{
    return wdist( x, cam + 320 ) + 320;
}

bool on_screen( float x, int w )
{
    float s = sxof( x );
    return s > -w && s < screen_width;
}

bool overlap( float ax, float ay, int aw, int ah, float bx, float by, int bw, int bh )
{
    float dx = wdist( bx, ax );
    return (dx < aw) && (-dx < bw) && (ay < by + bh) && (by < ay + ah);
}

void draw_rect( int x0, int y0, int x1, int y1 )
{
    if( x1 <= x0 || y1 <= y0 ) return;
    select_texture( 0 );
    select_region( REG_WHITE );
    set_drawing_scale( (x1 - x0) / 8.0, (y1 - y0) / 8.0 );
    draw_region_zoomed_at( x0, y0 );
}

void draw_sprite( int region, int x, int y )
{
    select_texture( 0 );
    select_region( region );
    draw_region_at( x, y );
}

void print_zoomed( int x, int y, int zoom, int* text )
{
    select_texture( -1 );
    set_drawing_scale( zoom, zoom );

    while( *text )
    {
        select_region( *text );
        draw_region_zoomed_at( x, y );
        x += bios_character_width * zoom;
        ++text;
    }

    select_texture( 0 );
}

void print_centered( int y, int zoom, int* text )
{
    int w = strlen( text ) * bios_character_width * zoom;
    print_zoomed( (screen_width - w) / 2, y, zoom, text );
}

void format_padded( int value, int digits, int* out )
{
    int[ 16 ] raw;
    itoa( value, raw, 10 );
    int pad = digits - strlen( raw );
    if( pad < 0 ) pad = 0;
    for( int i = 0; i < pad; ++i ) out[i] = '0';
    strcpy( out + pad, raw );
}

void sfx( int sound, int channel )
{
    play_sound_in_channel( sound, channel );
}

// -----------------------------------------------------------------------------
//   SCORE AND PARTICLES
// -----------------------------------------------------------------------------

void add_score( int points )
{
    score += points;
    if( score > 9999999 ) score = 9999999;

    // the cartridge awards a ship and a smart bomb together
    while( score >= next_bonus )
    {
        next_bonus += BONUS_EVERY;
        if( lives < 9 ) lives += 1;
        if( bombs < 9 ) bombs += 1;
        sfx( SND_EXTRA_LIFE, CH_JINGLE );
    }
}

int next_particle;

void burst( float x, float y, int count, int color, float speed )
{
    // particles are recycled round-robin, so a burst never scans the whole pool
    for( int n = 0; n < count; ++n )
    {
        int i = next_particle;
        next_particle = (next_particle + 1) % MAX_PARTICLES;
        float a = frnd( 0, 6.283 );
        float s = frnd( 0.3, speed );
        particles[i].alive = true;
        particles[i].x = x;
        particles[i].y = y;
        particles[i].vx = cos( a ) * s;
        particles[i].vy = sin( a ) * s;
        particles[i].life = 30 + rnd( 30 );
        particles[i].color = color;
    }
}

int type_color( int t )
{
    if( t == T_MINER )   return make_color_rgb( 255, 160, 50 );
    if( t == T_HUNTER )  return make_color_rgb( 110, 230, 90 );
    if( t == T_HIVE )    return make_color_rgb( 255, 80, 70 );
    if( t == T_STINGER ) return make_color_rgb( 255, 120, 120 );
    if( t == T_GRABBER ) return make_color_rgb( 120, 240, 100 );
    return make_color_rgb( 240, 90, 220 );
}

// -----------------------------------------------------------------------------
//   WORLD SETUP
// -----------------------------------------------------------------------------

void make_scenery()
{
    for( int i = 0; i < NUM_STARS; ++i )
    {
        stars[i].x = rnd( WORLD_W );
        stars[i].y = PF_T + rnd( GROUND_Y - PF_T - 10 );
        int b = 90 + rnd( 120 );
        stars[i].color = make_color_rgb( b, b, b + 30 );
    }

    for( int i = 0; i < NUM_BUILDINGS; ++i )
    {
        bld_w[i] = 26 + rnd( 14 );
        bld_h[i] = 12 + rnd( 36 );
        bld_win[i] = rnd( 256 );
    }
}

void reset_colonists()
{
    for( int i = 0; i < COLONISTS; ++i )
    {
        colonists[i].state = C_GROUND;
        colonists[i].x = (i * WORLD_W) / COLONISTS + rnd( 200 );
        colonists[i].y = GROUND_Y - H_COLONIST;
        colonists[i].vx = 0.15;
        if( rnd( 2 ) ) colonists[i].vx = -0.15;
    }
}

int colonists_left()
{
    int n = 0;
    for( int i = 0; i < COLONISTS; ++i )
      if( colonists[i].state != C_DEAD ) ++n;

    return n;
}

void enemy_size( int i )
{
    int t = enemies[i].type;
    if( t == T_MINER )   { enemies[i].w = W_MINER;   enemies[i].h = H_MINER; }
    if( t == T_HUNTER )  { enemies[i].w = W_HUNTER;  enemies[i].h = H_HUNTER; }
    if( t == T_HIVE )    { enemies[i].w = W_HIVE;    enemies[i].h = H_HIVE; }
    if( t == T_STINGER ) { enemies[i].w = W_STINGER; enemies[i].h = H_STINGER; }
    if( t == T_GRABBER ) { enemies[i].w = W_GRABBER; enemies[i].h = H_GRABBER; }
    if( t == T_RAVAGER ) { enemies[i].w = W_RAVAGER; enemies[i].h = H_RAVAGER; }
}

float enemy_speed()
{
    float s = 1.0 + 0.08 * (wave - 1);
    if( s > 2.2 ) s = 2.2;
    return s;
}

int fire_delay( int t )
{
    int base = 200;
    if( t == T_GRABBER ) base = 170;
    if( t == T_RAVAGER ) base = 100;
    if( t == T_HUNTER )  base = 60;
    if( t == T_STINGER ) base = 220;
    base -= (wave - 1) * 6;
    if( base < 40 ) base = 40;
    return base + rnd( base );
}

int spawn_enemy( int type, float x, float y, bool materialize )
{
    for( int i = 0; i < MAX_ENEMIES; ++i )
      if( !enemies[i].alive )
      {
          enemies[i].alive = true;
          enemies[i].type = type;
          enemies[i].x = wrap( x );
          enemies[i].y = y;
          enemies[i].vx = frnd( 0.6, 1.1 );
          if( rnd( 2 ) ) enemies[i].vx = -enemies[i].vx;
          enemies[i].vy = frnd( -0.6, 0.6 );
          enemies[i].spawn = 0;
          if( materialize ) enemies[i].spawn = 40;
          enemies[i].timer = rnd( 120 );
          enemies[i].fire_timer = fire_delay( type );
          enemies[i].target = -1;
          enemies[i].carrying = false;
          enemy_size( i );
          return i;
      }

    return -1;
}

// a random spot well away from the player
float far_x()
{
    return wrap( px + 500 + rnd( WORLD_W - 1000 ) );
}

bool hunter_dormant;

void start_wave()
{
    for( int i = 0; i < MAX_ENEMIES; ++i ) enemies[i].alive = false;
    for( int i = 0; i < MAX_SHOTS; ++i )   eshots[i].alive = false;
    for( int i = 0; i < MAX_MINES; ++i )   mines[i].alive = false;

    bool barren = !planet_alive;

    for( int s = 0; s < 7; ++s )
    {
        int t = wave_slots[s];
        if( barren ) t = T_RAVAGER;     // nothing left to abduct: every slot is a ravager
        spawn_enemy( t, far_x(), PF_T + 20 + rnd( 150 ), true );
    }

    hunter_dormant = true;
    if( barren )
    {
        spawn_enemy( T_RAVAGER, far_x(), PF_T + 20 + rnd( 150 ), true );
        hunter_dormant = false;
    }

    wave_frames = 0;
    laser_on = false;
    sfx( SND_MATERIALIZE, CH_SPAWN );
    game_state = GS_PLAY;
}

void place_player()
{
    py = (PF_T + GROUND_Y) / 2 - SHIP_H / 2;
    pvx = 0;
    facing = 1;
    anchor = 160;
    cam = wrap( px - anchor );
    in_city = false;
    bomb_armed = true;
    laser_on = false;
    laser_reload = 0;
    hyper_cooldown = 0;
    hyper_armed = true;
}

void start_game()
{
    int g = game_number - 1;
    srand( get_frame_counter() * 7919 + get_time() );
    make_scenery();
    score = 0;
    next_bonus = BONUS_EVERY;
    lives = START_LIVES - 1;
    bombs = START_BOMBS;
    wave = game_start_wave[ g ];
    paused = false;
    planet_alive = !game_barren[ g ];
    reset_colonists();
    if( !planet_alive )
      for( int i = 0; i < COLONISTS; ++i ) colonists[i].state = C_DEAD;

    for( int i = 0; i < MAX_PARTICLES; ++i ) particles[i].alive = false;
    px = 200;
    place_player();
    start_wave();
}

// -----------------------------------------------------------------------------
//   KILLING THINGS
// -----------------------------------------------------------------------------

void destroy_planet()
{
    planet_alive = false;
    planet_flash = 90;
    sfx( SND_BIG_BOOM, CH_BOOM2 );

    // every abductor goes berserk
    for( int i = 0; i < MAX_ENEMIES; ++i )
      if( enemies[i].alive && enemies[i].type == T_GRABBER )
      {
          enemies[i].type = T_RAVAGER;
          enemies[i].carrying = false;
          enemies[i].target = -1;
          enemy_size( i );
      }
}

void kill_enemy( int i, bool scores )
{
    if( !enemies[i].alive ) return;
    enemies[i].alive = false;
    float cx = enemies[i].x + enemies[i].w / 2;
    float cy = enemies[i].y + enemies[i].h / 2;
    burst( cx, cy, 14, type_color( enemies[i].type ), 2.5 );
    if( scores ) add_score( enemy_points[ enemies[i].type ] );
    sfx( SND_BOOM, CH_BOOM );

    // a colonist being carried starts to fall
    if( enemies[i].carrying )
    {
        int c = enemies[i].target;
        colonists[c].state = C_FALLING;
        colonists[c].vy = 0;
        colonists[c].fall_from = colonists[c].y;
    }

    // a hive bursts into stingers
    if( enemies[i].type == T_HIVE )
      for( int k = 0; k < STINGERS_PER_HIVE; ++k )
      {
          int s = spawn_enemy( T_STINGER, cx + frnd( -10, 10 ), cy + frnd( -10, 10 ), false );
          if( s >= 0 )
          {
              enemies[s].vx = frnd( -2, 2 );
              enemies[s].vy = frnd( -1.5, 1.5 );
              enemies[s].fire_timer = 90 + rnd( 120 );
          }
      }
}

void kill_player()
{
    game_state = GS_DEAD;
    state_timer = 0;
    laser_on = false;
    burst( px + SHIP_W / 2, py + SHIP_H / 2, 40, make_color_rgb( 120, 200, 255 ), 4.0 );
    burst( px + SHIP_W / 2, py + SHIP_H / 2, 30, color_white, 3.0 );
    sfx( SND_BIG_BOOM, CH_BOOM2 );

    // anything the ship was holding falls
    for( int c = 0; c < COLONISTS; ++c )
      if( colonists[c].state == C_HELD )
      {
          colonists[c].state = C_FALLING;
          colonists[c].vy = 0;
          colonists[c].fall_from = colonists[c].y;
      }
}

void smart_bomb()
{
    bombs -= 1;
    screen_flash = 12;
    sfx( SND_SMART_BOMB, CH_BOOM2 );

    for( int i = 0; i < MAX_ENEMIES; ++i )
      if( enemies[i].alive && on_screen( enemies[i].x, enemies[i].w ) )
        kill_enemy( i, true );

    for( int i = 0; i < MAX_SHOTS; ++i )
      if( eshots[i].alive && on_screen( eshots[i].x, 6 ) ) eshots[i].alive = false;

    for( int i = 0; i < MAX_MINES; ++i )
      if( mines[i].alive && on_screen( mines[i].x, 6 ) ) mines[i].alive = false;
}

void hyperspace()
{
    burst( px + SHIP_W / 2, py + SHIP_H / 2, 20, make_color_rgb( 120, 240, 255 ), 3.0 );
    px = rnd( WORLD_W );
    py = PF_T + 30 + rnd( GROUND_Y - PF_T - 80 );
    pvx = 0;
    anchor = 320 - facing * 160;
    cam = wrap( px - anchor );
    hyper_cooldown = 30;
    hyper_armed = false;
    screen_flash = 6;
    sfx( SND_HYPERSPACE, CH_EVENT );
}

// -----------------------------------------------------------------------------
//   PLAYER
// -----------------------------------------------------------------------------

void update_player()
{
    select_gamepad( 0 );
    int dx = 0, dy = 0;
    if( gamepad_left() > 0 )  dx = -1;
    if( gamepad_right() > 0 ) dx = 1;
    if( gamepad_up() > 0 )    dy = -1;
    if( gamepad_down() > 0 )  dy = 1;

    // reversing is instant, then the ship accelerates the other way
    if( dx != 0 ) facing = dx;
    thrusting = (dx != 0);

    if( thrusting )
    {
        pvx += facing * 0.32;
        pvx *= 0.985;
    }
    else pvx *= 0.95;

    pvx = fclamp( pvx, -7.5, 7.5 );
    px = wrap( px + pvx );
    py += dy * 3.2;

    // flying off the top of the sky is a hyperspace jump
    if( hyper_cooldown > 0 ) hyper_cooldown -= 1;
    if( py > PF_T + 24 ) hyper_armed = true;     // one jump per climb
    
    if( py < PF_T - 4 )
    {
        py = PF_T - 4;
        if( dy < 0 && hyper_cooldown == 0 && hyper_armed ) hyperspace();
    }

    if( py > CITY_B - SHIP_H ) py = CITY_B - SHIP_H;

    // diving into the city sets off a smart bomb (one per dive)
    in_city = (py + SHIP_H / 2 > GROUND_Y + 4);

    if( in_city && bomb_armed )
    {
        bomb_armed = false;
        if( bombs > 0 ) smart_bomb();
    }

    if( py + SHIP_H / 2 < GROUND_Y - 8 ) bomb_armed = true;

    // the camera keeps more room ahead of the ship
    float target = 160;
    if( facing < 0 ) target = 480 - SHIP_W;
    anchor += (target - anchor) * 0.05;
    cam = wrap( px - anchor );

    // laser
    if( laser_reload > 0 ) laser_reload -= 1;

    if( gamepad_button_a() > 0 && !laser_on && laser_reload == 0 && !in_city )
    {
        laser_on = true;
        laser_dir = facing;
        laser_y = py + 8;
        laser_x = px + SHIP_W - 4;
        if( facing < 0 ) laser_x = px + 4;
        laser_len = 0;
        sfx( SND_LASER, CH_LASER );
    }
}

void update_laser()
{
    if( !laser_on ) return;
    laser_len += 26;

    // nearest enemy along the beam is hit
    int best = -1;
    float best_d = laser_len;

    for( int i = 0; i < MAX_ENEMIES; ++i )
    {
        if( !enemies[i].alive || enemies[i].spawn > 0 ) continue;
        if( laser_y + 2 < enemies[i].y || laser_y > enemies[i].y + enemies[i].h ) continue;

        float d = wdist( enemies[i].x + enemies[i].w / 2, laser_x ) * laser_dir;
        d -= enemies[i].w / 2;
        if( d < -enemies[i].w ) continue;

        if( d < best_d )
        {
            best_d = d;
            best = i;
        }
    }

    if( best >= 0 )
    {
        kill_enemy( best, true );
        laser_on = false;
        laser_reload = 4;
        return;
    }

    if( laser_len > 640 )
    {
        laser_on = false;
        laser_reload = 4;
    }
}

// -----------------------------------------------------------------------------
//   ENEMIES
// -----------------------------------------------------------------------------

void enemy_fire( int i )
{
    float ex = enemies[i].x + enemies[i].w / 2;
    float ey = enemies[i].y + enemies[i].h / 2;
    float ddx = wdist( px + SHIP_W / 2, ex );
    float ddy = (py + SHIP_H / 2) - ey;
    float len = sqrt( ddx * ddx + ddy * ddy );
    if( len < 40 || len > 520 ) return;

    for( int s = 0; s < MAX_SHOTS; ++s )
      if( !eshots[s].alive )
      {
          float sp = 2.4 + 0.12 * wave;
          if( sp > 4.5 ) sp = 4.5;
          eshots[s].alive = true;
          eshots[s].x = ex - 3;
          eshots[s].y = ey - 3;
          eshots[s].vx = ddx / len * sp + pvx * 0.5;
          eshots[s].vy = ddy / len * sp;
          eshots[s].life = 180;
          sfx( SND_ENEMY_SHOT, CH_ESHOT );
          return;
      }
}

int free_colonist_near( float x )
{
    int best = -1;
    float best_d = 99999;

    for( int c = 0; c < COLONISTS; ++c )
    {
        if( colonists[c].state != C_GROUND ) continue;
        bool taken = false;

        for( int i = 0; i < MAX_ENEMIES; ++i )
          if( enemies[i].alive && enemies[i].target == c ) taken = true;

        if( taken ) continue;
        float d = fabs( wdist( colonists[c].x, x ) );
        if( d < best_d ) { best_d = d; best = c; }
    }

    return best;
}

void bounce_y( int i, float top, float bottom )
{
    if( enemies[i].y < top )    { enemies[i].y = top;    enemies[i].vy = fabs( enemies[i].vy ); }
    if( enemies[i].y > bottom ) { enemies[i].y = bottom; enemies[i].vy = -fabs( enemies[i].vy ); }
}

void update_grabber( int i, float sp )
{
    enemy* e = &enemies[i];

    if( e->carrying )
    {
        int c = e->target;
        e->y -= 0.7 * sp;
        colonists[c].x = e->x + e->w / 2 - W_COLONIST / 2;
        colonists[c].y = e->y + e->h - 2;

        // reached the top: the colonist is lost and the abductor mutates
        if( e->y <= PF_T - 2 )
        {
            colonists[c].state = C_DEAD;
            e->carrying = false;
            e->target = -1;
            e->type = T_RAVAGER;
            enemy_size( i );
            sfx( SND_MUTATE, CH_EVENT );
            if( colonists_left() == 0 && planet_alive ) destroy_planet();
        }
        return;
    }

    if( e->target >= 0 )
    {
        int c = e->target;
        if( colonists[c].state != C_GROUND ) { e->target = -1; return; }

        float d = wdist( colonists[c].x + W_COLONIST / 2, e->x + e->w / 2 );

        if( fabs( d ) > 3 )
        {
            e->x = wrap( e->x + fsign( d ) * fclamp( fabs( d ), 0, 1.5 * sp ) );
            if( e->y > GROUND_Y - 140 ) e->y -= 0.5;
            return;
        }

        e->y += 0.9 * sp;

        if( e->y + e->h >= colonists[c].y + 2 )
        {
            e->carrying = true;
            colonists[c].state = C_CARRIED;
            sfx( SND_ABDUCT, CH_EVENT );
        }
        return;
    }

    // wandering
    e->x = wrap( e->x + e->vx * sp );
    e->y += e->vy;
    bounce_y( i, PF_T + 6, GROUND_Y - 90 );
    e->timer += 1;

    if( e->timer > 150 )
    {
        e->timer = 0;
        if( rnd( 2 ) == 0 ) e->target = free_colonist_near( e->x );
        e->vy = frnd( -0.6, 0.6 );
    }
}

void update_enemies()
{
    float sp = enemy_speed();
    float pcx = px + SHIP_W / 2;
    float pcy = py + SHIP_H / 2;

    for( int i = 0; i < MAX_ENEMIES; ++i )
    {
        if( !enemies[i].alive ) continue;
        enemy* e = &enemies[i];

        if( e->spawn > 0 )
        {
            e->spawn -= 1;
            continue;
        }

        int t = e->type;
        float ecx = e->x + e->w / 2;
        float ecy = e->y + e->h / 2;
        float ddx = wdist( pcx, ecx );
        float ddy = pcy - ecy;

        if( t == T_GRABBER ) update_grabber( i, sp );

        else if( t == T_RAVAGER )
        {
            float speed = 1.5 * sp;
            if( fabs( ddx ) > 400 ) speed = 3.0 * sp;
            e->x = wrap( e->x + fclamp( ddx * 0.05, -speed, speed ) + frnd( -0.8, 0.8 ) );
            e->y += fclamp( ddy * 0.04, -1.3 * sp, 1.3 * sp ) + frnd( -1.4, 1.4 );
            bounce_y( i, PF_T, GROUND_Y - e->h );
        }

        else if( t == T_MINER )
        {
            e->x = wrap( e->x + e->vx * 1.3 * sp );
            if( e->vy == 0 ) e->vy = 0.7;
            e->y += e->vy;
            bounce_y( i, PF_T + 10, GROUND_Y - 50 );
            e->timer += 1;

            if( e->timer > 80 )
            {
                e->timer = 0;
                for( int m = 0; m < MAX_MINES; ++m )
                  if( !mines[m].alive )
                  {
                      mines[m].alive = true;
                      mines[m].x = ecx - W_MINE / 2;
                      mines[m].y = ecy - H_MINE / 2;
                      mines[m].life = 600;
                      break;
                  }
            }
        }

        else if( t == T_HIVE )
        {
            e->x = wrap( e->x + e->vx * 0.5 );
            e->y += e->vy * 0.4;
            bounce_y( i, PF_T + 10, GROUND_Y - 60 );
        }

        else if( t == T_STINGER )
        {
            e->vx = fclamp( e->vx + fsign( ddx ) * 0.10, -2.8 * sp, 2.8 * sp );
            e->vy = fclamp( e->vy + fsign( ddy ) * 0.08, -1.8, 1.8 );
            e->x = wrap( e->x + e->vx );
            e->y += e->vy;
            bounce_y( i, PF_T, GROUND_Y - e->h );
        }

        else if( t == T_HUNTER )
        {
            // keeps pace with the ship and closes in
            float want = fabs( pvx ) + 2.0 * sp;
            e->vx = fclamp( e->vx + fsign( ddx ) * 0.25, -want, want );
            e->x = wrap( e->x + e->vx );
            e->y += fclamp( ddy * 0.05, -2.0, 2.0 );
            bounce_y( i, PF_T, GROUND_Y - e->h );
        }

        // shooting
        if( t != T_MINER && t != T_HIVE && !e->carrying )
        {
            e->fire_timer -= 1;

            if( e->fire_timer <= 0 )
            {
                e->fire_timer = fire_delay( t );
                if( on_screen( e->x, e->w ) && game_state == GS_PLAY ) enemy_fire( i );
            }
        }
    }

    // the dormant hunter wakes if the wave drags on, then keeps coming back
    if( wave_frames >= HUNTER_WAKE_TIME && (wave_frames % 600) == 0 )
    {
        float hx = wrap( px - facing * 420 );
        spawn_enemy( T_HUNTER, hx, PF_T + 20 + rnd( 120 ), true );
        sfx( SND_MATERIALIZE, CH_SPAWN );
    }
}

bool player_hittable()
{
    // the cartridge blanks the ship while its shot is in flight,
    // and does not draw it inside the city
    return game_state == GS_PLAY && !laser_on && !in_city;
}

void update_hazards()
{
    // enemy shots
    for( int s = 0; s < MAX_SHOTS; ++s )
    {
        if( !eshots[s].alive ) continue;
        eshots[s].x = wrap( eshots[s].x + eshots[s].vx );
        eshots[s].y += eshots[s].vy;
        eshots[s].life -= 1;
        if( eshots[s].life <= 0 || eshots[s].y < PF_T - 6 || eshots[s].y > GROUND_Y ) eshots[s].alive = false;

        if( eshots[s].alive && player_hittable() && overlap( px + 4, py + 4, SHIP_W - 8, SHIP_H - 8, eshots[s].x, eshots[s].y, 6, 6 ) )
        {
            eshots[s].alive = false;
            kill_player();
        }
    }

    // mines
    for( int m = 0; m < MAX_MINES; ++m )
    {
        if( !mines[m].alive ) continue;
        mines[m].life -= 1;
        if( mines[m].life <= 0 ) mines[m].alive = false;

        if( mines[m].alive && player_hittable() && overlap( px + 4, py + 4, SHIP_W - 8, SHIP_H - 8, mines[m].x, mines[m].y, W_MINE, H_MINE ) )
        {
            mines[m].alive = false;
            kill_player();
        }
    }

    // ramming an enemy destroys both
    for( int i = 0; i < MAX_ENEMIES; ++i )
    {
        if( !enemies[i].alive || enemies[i].spawn > 0 || !player_hittable() ) continue;

        if( overlap( px + 3, py + 3, SHIP_W - 6, SHIP_H - 6, enemies[i].x + 2, enemies[i].y + 2, enemies[i].w - 4, enemies[i].h - 4 ) )
        {
            kill_enemy( i, true );
            kill_player();
        }
    }
}

// -----------------------------------------------------------------------------
//   COLONISTS
// -----------------------------------------------------------------------------

void update_colonists()
{
    for( int c = 0; c < COLONISTS; ++c )
    {
        colonist* k = &colonists[c];

        if( k->state == C_GROUND )
        {
            k->x = wrap( k->x + k->vx );
            k->y = GROUND_Y - H_COLONIST;
            if( rnd( 300 ) == 0 ) k->vx = -k->vx;
        }

        else if( k->state == C_FALLING )
        {
            k->vy = fclamp( k->vy + 0.035, 0, 2.2 );
            k->y += k->vy;

            // caught in mid-air
            if( game_state == GS_PLAY && overlap( px, py, SHIP_W, SHIP_H + 6, k->x, k->y, W_COLONIST, H_COLONIST ) )
            {
                k->state = C_HELD;
                add_score( 500 );
                sfx( SND_CATCH, CH_EVENT );
            }

            else if( k->y >= GROUND_Y - H_COLONIST )
            {
                k->y = GROUND_Y - H_COLONIST;

                if( GROUND_Y - k->fall_from > 120 )
                {
                    k->state = C_DEAD;
                    burst( k->x + 3, k->y + 7, 10, make_color_rgb( 255, 205, 160 ), 1.5 );
                    if( colonists_left() == 0 && planet_alive ) destroy_planet();
                }
                else
                {
                    k->state = C_GROUND;
                    add_score( 250 );
                }
            }
        }

        else if( k->state == C_HELD )
        {
            k->x = px + SHIP_W / 2 - W_COLONIST / 2;
            k->y = py + SHIP_H - 2;

            // set down safely near the ground
            if( k->y + H_COLONIST >= GROUND_Y - 4 )
            {
                k->state = C_GROUND;
                k->y = GROUND_Y - H_COLONIST;
                add_score( 500 );
                sfx( SND_CATCH, CH_EVENT );
            }
        }
    }
}

void update_particles()
{
    for( int i = 0; i < MAX_PARTICLES; ++i )
    {
        if( !particles[i].alive ) continue;
        particles[i].x = wrap( particles[i].x + particles[i].vx );
        particles[i].y += particles[i].vy;
        particles[i].vx *= 0.97;
        particles[i].vy *= 0.97;
        particles[i].life -= 1;
        if( particles[i].life <= 0 ) particles[i].alive = false;
    }
}

bool wave_cleared()
{
    for( int i = 0; i < MAX_ENEMIES; ++i )
      if( enemies[i].alive && enemies[i].type != T_HUNTER ) return false;

    return true;
}

// -----------------------------------------------------------------------------
//   DRAWING
// -----------------------------------------------------------------------------

void draw_stars()
{
    for( int i = 0; i < NUM_STARS; ++i )
    {
        // stars scroll at a third of the ground speed
        float sx = stars[i].x - cam * 0.33;
        while( sx < 0 ) sx += WORLD_W;
        while( sx >= WORLD_W ) sx -= WORLD_W;
        sx = sx * screen_width / 853.0;
        if( sx >= screen_width ) continue;
        set_multiply_color( stars[i].color );
        draw_rect( (int)sx, stars[i].y, (int)sx + 2, stars[i].y + 2 );
    }
}

void draw_city()
{
    if( !planet_alive )
    {
        // scorched ground only
        set_multiply_color( make_color_rgb( 60, 25, 20 ) );
        draw_rect( 0, CITY_B - 10, screen_width, CITY_B );
        return;
    }

    int spacing = WORLD_W / NUM_BUILDINGS;     // 40
    int first = (int)(cam / spacing);

    for( int k = -1; k < 18; ++k )
    {
        int b = (first + k + NUM_BUILDINGS) % NUM_BUILDINGS;
        float wx = (first + k) * spacing;
        int sx = (int)(wx - cam);
        int top = GROUND_Y + 50 - bld_h[b];

        set_multiply_color( make_color_rgb( 50, 40, 110 ) );
        draw_rect( sx, top, sx + bld_w[b], CITY_B );

        // lit windows
        set_multiply_color( make_color_rgb( 255, 220, 120 ) );
        int bits = bld_win[b];
        for( int r = 0; r < 4; ++r )
          for( int col = 0; col < 2; ++col )
            if( (bits >> (r * 2 + col)) & 1 )
            {
                int wy = top + 5 + r * 9;
                if( wy + 4 < CITY_B - 2 )
                  draw_rect( sx + 6 + col * 12, wy, sx + 10 + col * 12, wy + 4 );
            }
    }

    // ground surface line
    set_multiply_color( make_color_rgb( 80, 200, 120 ) );
    draw_rect( 0, GROUND_Y, screen_width, GROUND_Y + 2 );
}

int enemy_region( int t )
{
    if( t == T_MINER )   return REG_MINER;
    if( t == T_HUNTER )  return REG_HUNTER;
    if( t == T_HIVE )    return REG_HIVE;
    if( t == T_STINGER ) return REG_STINGER;
    if( t == T_GRABBER ) return REG_GRABBER;
    return REG_RAVAGER;
}

void draw_enemies()
{
    for( int i = 0; i < MAX_ENEMIES; ++i )
    {
        if( !enemies[i].alive ) continue;
        if( !on_screen( enemies[i].x, enemies[i].w ) ) continue;
        int sx = (int)sxof( enemies[i].x );
        int sy = (int)enemies[i].y;

        if( enemies[i].spawn > 0 )
        {
            // materializing: a shrinking cloud of sparks
            int s = enemies[i].spawn;
            set_multiply_color( type_color( enemies[i].type ) );
            for( int k = 0; k < 6; ++k )
            {
                float a = k * 1.047 + s * 0.15;
                int ox = (int)(cos( a ) * s * 0.8) + enemies[i].w / 2;
                int oy = (int)(sin( a ) * s * 0.8) + enemies[i].h / 2;
                draw_rect( sx + ox, sy + oy, sx + ox + 2, sy + oy + 2 );
            }
            continue;
        }

        set_multiply_color( color_white );
        draw_sprite( enemy_region( enemies[i].type ), sx, sy );
    }

    set_multiply_color( color_white );
    int blink = get_frame_counter() % 16;

    for( int m = 0; m < MAX_MINES; ++m )
      if( mines[m].alive && on_screen( mines[m].x, W_MINE ) )
        if( mines[m].life > 90 || blink < 8 )
          draw_sprite( REG_MINE, (int)sxof( mines[m].x ), (int)mines[m].y );

    for( int s = 0; s < MAX_SHOTS; ++s )
      if( eshots[s].alive && on_screen( eshots[s].x, 6 ) )
        draw_sprite( REG_BULLET, (int)sxof( eshots[s].x ), (int)eshots[s].y );
}

void draw_colonists()
{
    set_multiply_color( color_white );

    for( int c = 0; c < COLONISTS; ++c )
    {
        if( colonists[c].state == C_DEAD ) continue;
        if( !on_screen( colonists[c].x, W_COLONIST ) ) continue;
        draw_sprite( REG_COLONIST, (int)sxof( colonists[c].x ), (int)colonists[c].y );
    }
}

void draw_player()
{
    if( game_state != GS_PLAY && game_state != GS_WAVEEND ) return;
    int sx = (int)sxof( px );
    int sy = (int)py;
    int ship = REG_SHIP_R;
    if( facing < 0 ) ship = REG_SHIP_L;

    // faint while the laser is out or inside the city (can't be hit)
    if( laser_on || in_city ) set_multiply_color( make_color_rgba( 255, 255, 255, 70 ) );
    else set_multiply_color( color_white );
    draw_sprite( ship, sx, sy );

    if( thrusting )
    {
        int f = (get_frame_counter() / 3) % 2;
        if( facing > 0 ) draw_sprite( REG_FLAME0_R + f, sx - W_FLAME0_R + 2, sy + 6 );
        else             draw_sprite( REG_FLAME0_L + f, sx + SHIP_W - 2, sy + 6 );
    }

    if( laser_on )
    {
        float x0 = sxof( laser_x );
        float x1 = x0 + laser_dir * laser_len;
        if( x1 < x0 ) { float t = x0; x0 = x1; x1 = t; }
        set_multiply_color( make_color_rgb( 255, 255, 255 ) );
        draw_rect( (int)x0, (int)laser_y, (int)x1, (int)laser_y + 2 );
        set_multiply_color( make_color_rgb( 110, 240, 255 ) );
        draw_rect( (int)x0, (int)laser_y - 1, (int)x1, (int)laser_y );
        draw_rect( (int)x0, (int)laser_y + 2, (int)x1, (int)laser_y + 3 );
    }
}

void draw_particles()
{
    for( int i = 0; i < MAX_PARTICLES; ++i )
    {
        if( !particles[i].alive ) continue;
        if( !on_screen( particles[i].x, 3 ) ) continue;
        set_multiply_color( particles[i].color );
        int sx = (int)sxof( particles[i].x );
        int sy = (int)particles[i].y;
        draw_rect( sx, sy, sx + 3, sy + 3 );
    }
}

void scanner_dot( float x, float y, int color, int size )
{
    float rel = wdist( x, cam + 320 );            // keep the view centered
    int dx = SCAN_X + SCAN_W / 2 + (int)(rel * SCAN_W / WORLD_W);
    int dy = SCAN_Y + 2 + (int)((y - PF_T + 6) * (SCAN_H - 6) / (CITY_B - PF_T + 6));
    set_multiply_color( color );
    draw_rect( dx, dy, dx + size, dy + size );
}

void draw_scanner()
{
    set_multiply_color( make_color_rgb( 10, 14, 40 ) );
    draw_rect( SCAN_X, SCAN_Y, SCAN_X + SCAN_W, SCAN_Y + SCAN_H );
    set_multiply_color( make_color_rgb( 70, 110, 255 ) );
    draw_rect( SCAN_X - 2, SCAN_Y - 2, SCAN_X + SCAN_W + 2, SCAN_Y );
    draw_rect( SCAN_X - 2, SCAN_Y + SCAN_H, SCAN_X + SCAN_W + 2, SCAN_Y + SCAN_H + 2 );
    draw_rect( SCAN_X - 2, SCAN_Y, SCAN_X, SCAN_Y + SCAN_H );
    draw_rect( SCAN_X + SCAN_W, SCAN_Y, SCAN_X + SCAN_W + 2, SCAN_Y + SCAN_H );

    // the visible window
    int vw = screen_width * SCAN_W / WORLD_W;
    int vx = SCAN_X + SCAN_W / 2 - vw / 2;
    set_multiply_color( make_color_rgb( 160, 160, 190 ) );
    draw_rect( vx, SCAN_Y, vx + vw, SCAN_Y + 1 );
    draw_rect( vx, SCAN_Y + SCAN_H - 1, vx + vw, SCAN_Y + SCAN_H );

    if( planet_alive )
    {
        int gy = SCAN_Y + 2 + (GROUND_Y - PF_T + 6) * (SCAN_H - 6) / (CITY_B - PF_T + 6);
        set_multiply_color( make_color_rgb( 40, 120, 70 ) );
        draw_rect( SCAN_X, gy, SCAN_X + SCAN_W, gy + 1 );
    }

    for( int c = 0; c < COLONISTS; ++c )
      if( colonists[c].state != C_DEAD )
        scanner_dot( colonists[c].x, colonists[c].y, make_color_rgb( 255, 205, 160 ), 2 );

    for( int i = 0; i < MAX_ENEMIES; ++i )
      if( enemies[i].alive )
        scanner_dot( enemies[i].x, enemies[i].y, type_color( enemies[i].type ), 3 );

    if( game_state != GS_DEAD )
      scanner_dot( px, py, color_white, 4 );
}

void draw_hud()
{
    int[ 16 ] num;
    set_multiply_color( make_color_rgb( 255, 220, 90 ) );
    format_padded( score, 7, num );
    print_zoomed( 12, 8, 1, num );

    // reserve ships and smart bombs
    set_multiply_color( color_white );
    int shown = lives;
    if( shown > 5 ) shown = 5;

    for( int i = 0; i < shown; ++i )
    {
        select_texture( 0 );
        select_region( REG_SHIP_R );
        set_drawing_scale( 0.5, 0.5 );
        draw_region_zoomed_at( 12 + i * 20, 32 );
    }

    shown = bombs;
    if( shown > 6 ) shown = 6;
    for( int i = 0; i < shown; ++i ) draw_sprite( REG_BOMBICON, 120 + i * 11, 33 );

    int[ 24 ] line;
    strcpy( line, "WAVE " );
    itoa( wave, num, 10 );
    strcat( line, num );
    set_multiply_color( make_color_rgb( 150, 150, 170 ) );
    print_zoomed( 500, 8, 1, line );

    strcpy( line, "HI " );
    format_padded( high_score, 7, num );
    strcat( line, num );
    print_zoomed( 500, 30, 1, line );

    if( paused )
    {
        set_multiply_color( color_white );
        print_centered( 160, 3, "PAUSED" );
    }
}

void draw_world()
{
    clear_screen( color_black );

    if( planet_flash > 0 )
    {
        planet_flash -= 1;
        if( (planet_flash / 4) % 2 == 0 ) clear_screen( make_color_rgb( 90, 20, 20 ) );
    }

    draw_stars();
    draw_city();
    draw_colonists();
    draw_enemies();
    draw_player();
    draw_particles();
    draw_scanner();
    draw_hud();

    if( screen_flash > 0 )
    {
        set_multiply_color( make_color_rgba( 255, 255, 255, screen_flash * 18 ) );
        draw_rect( 0, PF_T - 6, screen_width, CITY_B );
        screen_flash -= 1;
    }

    if( planet_flash > 30 )
    {
        set_multiply_color( make_color_rgb( 255, 90, 90 ) );
        print_centered( 160, 2, "THE PLANET IS LOST" );
    }
}

// -----------------------------------------------------------------------------
//   GAME STATES
// -----------------------------------------------------------------------------

void run_play()
{
    select_gamepad( 0 );

    if( gamepad_button_start() == 1 )
    {
        paused = !paused;
        if( paused ) pause_all_channels();
        else resume_all_channels();
    }

    if( !paused )
    {
        wave_frames += 1;
        if( game_state == GS_PLAY ) update_player();
        update_laser();
        update_enemies();
        update_hazards();
        update_colonists();
        update_particles();

        if( game_state == GS_PLAY && wave_cleared() )
        {
            game_state = GS_WAVEEND;
            state_timer = 0;
            laser_on = false;
            sfx( SND_WAVE_CLEAR, CH_JINGLE );
        }
    }

    draw_world();
}

void run_dead()
{
    state_timer += 1;
    wave_frames += 1;
    update_enemies();
    update_colonists();
    update_particles();
    draw_world();

    if( state_timer < 120 ) return;
    lives -= 1;

    if( lives < 0 )
    {
        if( score > high_score ) high_score = score;
        game_state = GS_GAMEOVER;
        state_timer = 0;
        return;
    }

    // clear the area around the respawn point
    for( int s = 0; s < MAX_SHOTS; ++s ) eshots[s].alive = false;
    for( int m = 0; m < MAX_MINES; ++m ) mines[m].alive = false;

    for( int i = 0; i < MAX_ENEMIES; ++i )
      if( enemies[i].alive && fabs( wdist( enemies[i].x, px ) ) < 400 && !enemies[i].carrying )
        enemies[i].x = far_x();

    place_player();
    game_state = GS_PLAY;
}

void run_wave_end()
{
    state_timer += 1;
    update_player();
    update_colonists();
    update_particles();
    draw_world();

    int[ 16 ] num;
    int[ 32 ] line;
    strcpy( line, "WAVE " );
    itoa( wave, num, 10 );
    strcat( line, num );
    strcat( line, " CLEARED" );
    set_multiply_color( make_color_rgb( 255, 220, 90 ) );
    print_centered( 130, 3, line );

    // the planet is rebuilt every fifth wave (unless the game starts barren)
    if( (wave % 5) == 0 && !game_barren[ game_number - 1 ] )
    {
        set_multiply_color( make_color_rgb( 120, 255, 140 ) );
        print_centered( 190, 2, "PLANET RESTORED" );
    }

    if( state_timer < 150 ) return;

    for( int s = 0; s < MAX_SHOTS; ++s ) eshots[s].alive = false;

    if( (wave % 5) == 0 && !game_barren[ game_number - 1 ] )
    {
        planet_alive = true;
        reset_colonists();
    }

    wave += 1;
    start_wave();
}

void run_gameover()
{
    state_timer += 1;
    update_enemies();
    update_particles();
    draw_world();

    set_multiply_color( make_color_rgb( 255, 80, 80 ) );
    print_centered( 140, 4, "GAME OVER" );

    select_gamepad( 0 );
    if( state_timer > 90 && (gamepad_button_start() == 1 || gamepad_button_a() == 1) )
    {
        game_state = GS_TITLE;
        title_frames = 0;
    }

    if( state_timer > 60 * 20 ) game_state = GS_TITLE;
}

void run_title()
{
    select_gamepad( 0 );
    title_frames += 1;

    if( gamepad_left() == 1 || gamepad_down() == 1 )
    {
        game_number -= 1;
        if( game_number < 1 ) game_number = NUM_GAMES;
        sfx( SND_BLIP, CH_JINGLE );
    }

    if( gamepad_right() == 1 || gamepad_up() == 1 )
    {
        game_number += 1;
        if( game_number > NUM_GAMES ) game_number = 1;
        sfx( SND_BLIP, CH_JINGLE );
    }

    if( (gamepad_button_start() == 1 || gamepad_button_a() == 1) && title_frames > 20 )
    {
        start_game();
        return;
    }

    // demo background: the sky scrolls by
    cam = wrap( cam + 1.5 );
    clear_screen( color_black );
    draw_stars();
    draw_city();

    int c = (title_frames / 3) % 64;
    int glow = (63 - c) * 3;
    if( c < 32 ) glow = c * 3;
    set_multiply_color( make_color_rgb( 110 + glow, 220, 255 ) );
    print_centered( 18, 4, "SKYWARDEN" );

    // cast of characters
    set_multiply_color( color_white );
    int y = 140;
    draw_sprite( REG_GRABBER, 150, y );       draw_sprite( REG_RAVAGER, 330, y );
    draw_sprite( REG_MINER, 150, y + 30 );    draw_sprite( REG_HIVE, 330, y + 28 );
    draw_sprite( REG_STINGER, 150, y + 62 );  draw_sprite( REG_HUNTER, 324, y + 62 );

    set_multiply_color( make_color_rgb( 200, 200, 220 ) );
    print_zoomed( 185, y + 2, 1, "GRABBER 150" );
    print_zoomed( 365, y + 2, 1, "RAVAGER 150" );
    print_zoomed( 185, y + 32, 1, "MINER   250" );
    print_zoomed( 365, y + 32, 1, "HIVE   1000" );
    print_zoomed( 185, y + 62, 1, "STINGER 500" );
    print_zoomed( 365, y + 62, 1, "HUNTER  200" );

    int[ 16 ] num;
    int[ 40 ] line;
    strcpy( line, "< GAME " );
    itoa( game_number, num, 10 );
    strcat( line, num );
    strcat( line, " >" );
    set_multiply_color( color_white );
    print_centered( 102, 2, line );

    strcpy( line, "START AT WAVE " );
    itoa( game_start_wave[ game_number - 1 ], num, 10 );
    strcat( line, num );
    if( game_barren[ game_number - 1 ] ) strcat( line, " - BARREN PLANET" );
    set_multiply_color( make_color_rgb( 170, 190, 255 ) );
    print_centered( 232, 1, line );

    if( (title_frames / 30) % 2 == 0 )
    {
        set_multiply_color( color_white );
        print_centered( 258, 1, "PRESS START" );
    }

    if( high_score > 0 )
    {
        strcpy( line, "HIGH SCORE " );
        format_padded( high_score, 7, num );
        strcat( line, num );
        set_multiply_color( make_color_rgb( 255, 220, 90 ) );
        print_centered( 8, 1, line );
    }
}

// -----------------------------------------------------------------------------
//   MAIN
// -----------------------------------------------------------------------------

void main( void )
{
    define_regions();
    set_blending_mode( blending_alpha );

    game_state = GS_TITLE;
    game_number = 1;
    planet_alive = true;
    cam = 0;
    srand( 1234 );
    make_scenery();

    while( true )
    {
        if( game_state == GS_TITLE )         run_title();
        else if( game_state == GS_PLAY )     run_play();
        else if( game_state == GS_DEAD )     run_dead();
        else if( game_state == GS_WAVEEND )  run_wave_end();
        else if( game_state == GS_GAMEOVER ) run_gameover();

        end_frame();
    }
}
