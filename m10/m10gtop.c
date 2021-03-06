
/* big endian forest
 *
 * gcc m1x12_20170424.c -lm
 * 2017-04-24 Ury
 * M10 w/ G.top GPS
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef CYGWIN
  #include <fcntl.h>  // cygwin: _setmode()
  #include <io.h>
#endif

typedef unsigned char ui8_t;
typedef unsigned short ui16_t;

typedef struct {
    int lat; int lon; int alt;
    int vE; int vN; int vU;
    double vH; double vD; double vV;
    int date; int time;
    char SN[12];
} datum_t;

datum_t datum;

int option_verbose = 0,  // ausfuehrliche Anzeige
    option_raw = 0,      // rohe Frames
    option_inv = 0,      // invertiert Signal
    option_res = 0,      // genauere Bitmessung
    option_b = 0,
    option_color = 0,
    wavloaded = 0;
int wav_channel = 0;     // audio channel: left


/* -------------------------------------------------------------------------- */

#define BAUD_RATE   9616 //2*4800

int sample_rate = 0, bits_sample = 0, channels = 0;
float samples_per_bit = 0;

int findstr(char *buf, char *str, int pos) {
    int i;
    for (i = 0; i < 4; i++) {
        if (buf[(pos+i)%4] != str[i]) break;
    }
    return i;
}

int read_wav_header(FILE *fp) {
    char txt[4+1] = "\0\0\0\0";
    unsigned char dat[4];
    int byte, p=0;

    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "RIFF", 4)) return -1;
    if (fread(txt, 1, 4, fp) < 4) return -1;
    // pos_WAVE = 8L
    if (fread(txt, 1, 4, fp) < 4) return -1;
    if (strncmp(txt, "WAVE", 4)) return -1;
    // pos_fmt = 12L
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "fmt ", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;

    if (fread(dat, 1, 2, fp) < 2) return -1;
    channels = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    memcpy(&sample_rate, dat, 4); //sample_rate = dat[0]|(dat[1]<<8)|(dat[2]<<16)|(dat[3]<<24);

    if (fread(dat, 1, 4, fp) < 4) return -1;
    if (fread(dat, 1, 2, fp) < 2) return -1;
    //byte = dat[0] + (dat[1] << 8);

    if (fread(dat, 1, 2, fp) < 2) return -1;
    bits_sample = dat[0] + (dat[1] << 8);

    // pos_dat = 36L + info
    for ( ; ; ) {
        if ( (byte=fgetc(fp)) == EOF ) return -1;
        txt[p % 4] = byte;
        p++; if (p==4) p=0;
        if (findstr(txt, "data", p) == 4) break;
    }
    if (fread(dat, 1, 4, fp) < 4) return -1;


    fprintf(stderr, "sample_rate: %d\n", sample_rate);
    fprintf(stderr, "bits       : %d\n", bits_sample);
    fprintf(stderr, "channels   : %d\n", channels);

    if ((bits_sample != 8) && (bits_sample != 16)) return -1;

    samples_per_bit = sample_rate/(float)BAUD_RATE;

    fprintf(stderr, "samples/bit: %.2f\n", samples_per_bit);

    return 0;
}


#define EOF_INT  0x1000000

unsigned long sample_count = 0;
double bitgrenze = 0;

int read_signed_sample(FILE *fp) {  // int = i32_t
    int byte, i, sample=0, s=0;     // EOF -> 0x1000000

    for (i = 0; i < channels; i++) {
                           // i = 0: links bzw. mono
        byte = fgetc(fp);
        if (byte == EOF) return EOF_INT;
        if (i == wav_channel) sample = byte;

        if (bits_sample == 16) {
            byte = fgetc(fp);
            if (byte == EOF) return EOF_INT;
            if (i == wav_channel) sample +=  byte << 8;
        }

    }

    if (bits_sample ==  8)  s = sample-128;   // 8bit: 00..FF, centerpoint 0x80=128
    if (bits_sample == 16)  s = (short)sample;

    sample_count++;

    return s;
}

int par=1, par_alt=1;

int read_bits_fsk(FILE *fp, int *bit, int *len) {
    static int sample;
    int n, y0;
    float l, x1;
    static float x0;

    n = 0;
    do{
        y0 = sample;
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        par_alt = par;
        par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        n++;
    } while (par*par_alt > 0);

    if (!option_res) l = (float)n / samples_per_bit;
    else {                                 // genauere Bitlaengen-Messung
        x1 = sample/(float)(sample-y0);    // hilft bei niedriger sample rate
        l = (n+x0-x1) / samples_per_bit;   // meist mehr frames (nicht immer)
        x0 = x1;
    }

    *len = (int)(l+0.5);

    if (!option_inv) *bit = (1+par_alt)/2;  // oben 1, unten -1
    else             *bit = (1-par_alt)/2;  // sdr#<rev1381?, invers: unten 1, oben -1
// *bit = (1+inv*par_alt)/2; // ausser inv=0

    /* Y-offset ? */

    return 0;
}

int bitstart = 0;
unsigned long scount = 0;
int read_rawbit(FILE *fp, int *bit) {
    int sample;
    int sum;

    sum = 0;

    if (bitstart) {
        scount = 0;    // eigentlich scount = 1
        bitgrenze = 0; //   oder bitgrenze = -1
        bitstart = 0;
    }
    bitgrenze += samples_per_bit;

    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum += sample;
        scount++;
    } while (scount < bitgrenze);  // n < samples_per_bit

    if (sum >= 0) *bit = 1;
    else          *bit = 0;

    if (option_inv) *bit ^= 1;

    return 0;
}

int read_rawbit2(FILE *fp, int *bit) {
    int sample;
    int sum;

    sum = 0;

    if (bitstart) {
        scount = 0;    // eigentlich scount = 1
        bitgrenze = 0; //   oder bitgrenze = -1
        bitstart = 0;
    }

    bitgrenze += samples_per_bit;
    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum += sample;
        scount++;
    } while (scount < bitgrenze);  // n < samples_per_bit

    bitgrenze += samples_per_bit;
    do {
        sample = read_signed_sample(fp);
        if (sample == EOF_INT) return EOF;
        //sample_count++; // in read_signed_sample()
        //par =  (sample >= 0) ? 1 : -1;    // 8bit: 0..127,128..255 (-128..-1,0..127)
        sum -= sample;
        scount++;
    } while (scount < bitgrenze);  // n < samples_per_bit

    if (sum >= 0) *bit = 1;
    else          *bit = 0;

    if (option_inv) *bit ^= 1;

    return 0;
}

/* -------------------------------------------------------------------------- */

/*
Header = Sync-Header + Sonde-Header:
1100110011001100 1010011001001100  1101010011010011 0100110101010101 0011010011001100
uudduudduudduudd ududduuddudduudd  uudududduududduu dudduudududududu dduududduudduudd (oder:)
dduudduudduudduu duduudduuduudduu  ddududuudduduudd uduuddududududud uudduduudduudduu (komplement)
 0 0 0 0 0 0 0 0  1 1 - - - 0 0 0   0 1 1 0 0 1 0 0  1 0 0 1 1 1 1 1  0 0 1 0 0 0 0 0
*/

#define BITS 8
#define HEADLEN 32  // HEADLEN+HEADOFS=32 <= strlen(header)
#define HEADOFS  0
                 // Sync-Header (raw)               // Sonde-Header (bits)
//char head[] = "11001100110011001010011001001100"; //"011001001001111100100000"; // M10: 64 9F 20 , M2K2: 64 8F 20
                                                    //"011101101001111100100000"; // M10: 76 9F 20 , aux-data?
                                                    //"011001000100100100001001"; // M10-dop: 64 49 09
                                                    //"011001001010111100000010"; // M10gtop: 64 AF 02
char header[] =  "10011001100110010100110010011001";

#define FRAME_LEN       (100+1)   // 0x64+1
#define BITFRAME_LEN    (FRAME_LEN*BITS)
#define RAWBITFRAME_LEN (BITFRAME_LEN*2)

char buf[HEADLEN];
int bufpos = -1;


#define FRAMESTART 0
#define AUX_LEN        20
#define BITAUX_LEN    (AUX_LEN*BITS)
#define RAWBITAUX_LEN (BITAUX_LEN*2)

ui8_t frame_bytes[FRAME_LEN+AUX_LEN+2];
char frame_rawbits[RAWBITFRAME_LEN+RAWBITAUX_LEN+16];  // frame_rawbits-32="11001100110011001010011001001100";
char frame_bits[BITFRAME_LEN+BITAUX_LEN+8];

int auxlen = 0; // 0 .. 0x76-0x64


void inc_bufpos() {
  bufpos = (bufpos+1) % HEADLEN;
}

char cb_inv(char c) {
    if (c == '0') return '1';
    if (c == '1') return '0';
    return c;
}

// Gefahr bei Manchester-Codierung: inverser Header wird leicht fehl-erkannt
// da manchester1 und manchester2 nur um 1 bit verschoben
int compare2() {
    int i, j;

    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != header[HEADOFS+HEADLEN-1-i]) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return 1;

    i = 0;
    j = bufpos;
    while (i < HEADLEN) {
        if (j < 0) j = HEADLEN-1;
        if (buf[j] != cb_inv(header[HEADOFS+HEADLEN-1-i])) break;
        j--;
        i++;
    }
    if (i == HEADLEN) return -1;

    return 0;

}

int bits2bytes(char *bitstr, ui8_t *bytes) {
    int i, bit, d, byteval;
    int bitpos, bytepos;

    bitpos = 0;
    bytepos = 0;

    while (bytepos < FRAME_LEN+AUX_LEN) {

        byteval = 0;
        d = 1;
        for (i = 0; i < BITS; i++) {
            //bit=*(bitstr+bitpos+i); /* little endian */
            bit=*(bitstr+bitpos+7-i);  /* big endian */
            // bit == 'x' ?
            if         (bit == '1')                     byteval += d;
            else /*if ((bit == '0') || (bit == 'x'))*/  byteval += 0;
            d <<= 1;
        }
        bitpos += BITS;
        bytes[bytepos++] = byteval & 0xFF;

    }

    //while (bytepos < FRAME_LEN+AUX_LEN) bytes[bytepos++] = 0;

    return 0;
}

/* -------------------------------------------------------------------------- */
// PSK  (bzw. biphase-M (oder differential Manchester?))
// nach Synchronisation: 00,11->0 ; 01,10->1 (Phasenwechsel)
void psk_bpm(char* frame_rawbits, char *frame_bits) {
    int i;
    char bit;
    //int err = 0;

    for (i = 0; i < BITFRAME_LEN+BITAUX_LEN; i++) {

        //if (i > 0 && (frame_rawbits[2*i] == frame_rawbits[2*i-1])) err = 1;

        if (frame_rawbits[2*i] == frame_rawbits[2*i+1]) bit = '0';
        else                                            bit = '1';

        //if (err) frame_bits[i] = 'x'; else
        frame_bits[i] = bit;
        //err = 0;

    }
}

/*
Header = Sync-Header + Sonde-Header:
1100110011001100 1010011001001100  1101010011010011 0100110101010101 0011010011001100
uudduudduudduudd ududduuddudduudd  uudududduududduu dudduudududududu dduududduudduudd (oder:)
dduudduudduudduu duduudduuduudduu  ddududuudduduudd uduuddududududud uudduduudduudduu (komplement)
 0 0 0 0 0 0 0 0  1 1 - - - 0 0 0   0 1 1 0 0 1 0 0  1 0 0 1 1 1 1 1  0 0 1 0 0 0 0 0
*/
/*
110101001101001101001101010101010011010011001100
 101010011010011010011010101010100110100110011001011001100101011001100110011001100110011001100101100110011001100110011001100101010101010101010101010101101010100110011010011001011001011010101010010101100110100110010101100110011001011001100110100101011010101001100101010101101010011001010101100110010110100110010101100101100101100110101010011001100110010110011001100110011001100110100101011010101001101010100101100101100110011001100110011001100110011001100110011001011001101010011001100110011010101001100101100110100110011001010101101001100110010110011010100110100110011010100110011001010101101001100110010101100110011010101001100110101001100110011001100101100110011001100110011001100110011001100110011001100110011001100110011001100110011001100110011010011010100110100110011001100110011001100110011001100101101001011010100101101001101001100110100110100110010110010110101010010110010110011001011001010101010101010110011001100110011010011010100110011001011001100110011001100110011001100110011001100110011001100110101001011010011010010110010110010101010110010110011001101001101010101010011010100110011010011010011010100110101001100110011010100110010110011001010110101001101001100110100101011001100110011010011001100110011001100110010101011001100110011001100110100110101001100110011001100110011001100110011001100110011001100110011001100110011001100110011001100110011010101001101001011001100101010101100110101001011001100110011001100110101001010110011001100110010110011001010110011001100110011001100110011001011001100101011010100101100110010110101001101001011001101001011001101010011010010110101001010101100110011001101010100110011001100000
*/
int dpsk_bpm(char* frame_rawbits, char *frame_bits, int len) {
    int i;
    char bit;
    char bit0;
    //int err = 0;

    bit0 = (frame_rawbits[0] & 1) ^ 1;

    for (i = 0; i < len/2; i++) {

        if ((frame_rawbits[2*i  ] & 1) == 1 &&
            (frame_rawbits[2*i+1] & 1) == 0   ) bit = 1;
   else if ((frame_rawbits[2*i  ] & 1) == 0 &&
            (frame_rawbits[2*i+1] & 1) == 1   ) bit = 0;
        else {
                bit = 2;
                frame_bits[i] = 'x';
                bit0 = bit&1;
                continue;
                //err = 1;
             }

        if (bit0 == bit) frame_bits[i] = '1';
        else             frame_bits[i] = '0';
// frame_bits[i] = 0x31 ^ (bit0 ^ bit);

        bit0 = bit;

    }

    return bit0;
}

/* -------------------------------------------------------------------------- */

#define stdFLEN        0x64  // pos[0]=0x64
#define pos_GPSlat     0x04  // 4 byte
#define pos_GPSlon     0x08  // 4 byte
#define pos_GPSalt     0x0C  // 3 byte
#define pos_GPSvE      0x0F  // 2 byte
#define pos_GPSvN      0x11  // 2 byte
#define pos_GPSvU      0x13  // 2 byte
#define pos_GPStime    0x15  // 3 byte
#define pos_GPSdate    0x18  // 3 byte

#define pos_SN         0x5D  // 2+3 byte
#define pos_Check     (stdFLEN-1)  // 2 byte

#define ANSI_COLOR_RESET   "\x1b[0m"

#define col_GPSdate    "\x1b[38;5;20m"  // 3 byte
#define col_GPStime    "\x1b[38;5;27m"  // 3 byte
#define col_GPSlat     "\x1b[38;5;34m"  // 4 byte
#define col_GPSlon     "\x1b[38;5;70m"  // 4 byte
#define col_GPSalt     "\x1b[38;5;82m"  // 4 byte
#define col_GPSvel     "\x1b[38;5;36m"  // 6 byte
#define col_SN         "\x1b[38;5;58m"  // 3 byte
#define col_Check      "\x1b[38;5;11m"  // 2 byte
#define col_TXT        "\x1b[38;5;244m"
#define col_FRTXT      "\x1b[38;5;244m"
#define col_CSok       "\x1b[38;5;2m"
#define col_CSno       "\x1b[38;5;1m"

/* -------------------------------------------------------------------------- */


int get_GPSpos() {
    int i;
    ui8_t bytes[4];
    int val;

    for (i = 0; i < 4; i++)  bytes[i] = frame_bytes[pos_GPSlat + i];
    val = 0;
    for (i = 0; i < 4; i++)  val |= bytes[i] << (8*(3-i));
    datum.lat = val;

    for (i = 0; i < 4; i++)  bytes[i] = frame_bytes[pos_GPSlon + i];
    val = 0;
    for (i = 0; i < 4; i++)  val |= bytes[i] << (8*(3-i));
    datum.lon = val;

    for (i = 0; i < 3; i++)  bytes[i] = frame_bytes[pos_GPSalt + i];
    val = 0;
    for (i = 0; i < 3; i++)  val |= bytes[i] << (8*(2-i));
    if (val & 0x800000) val -= 0x1000000; // alt: signed 24bit?
    datum.alt = val;

    return 0;
}

int get_GPSvel() {
    int i;
    ui8_t bytes[2];
    short vel16;
    double vx, vy, vz, dir;

    for (i = 0; i < 2; i++)  bytes[i] = frame_bytes[pos_GPSvE + i];
    vel16 = bytes[0] << 8 | bytes[1];
    datum.vE = vel16;
    vx = vel16 / 1e2; // east

    for (i = 0; i < 2; i++)  bytes[i] = frame_bytes[pos_GPSvN + i];
    vel16 = bytes[0] << 8 | bytes[1];
    datum.vN = vel16;
    vy= vel16 / 1e2; // north

    for (i = 0; i < 2; i++)  bytes[i] = frame_bytes[pos_GPSvU + i];
    vel16 = bytes[0] << 8 | bytes[1];
    datum.vU = vel16;
    vz = vel16 / 1e2; // up

    datum.vH = sqrt(vx*vx+vy*vy);
    dir = atan2(vx, vy) * 180 / M_PI;
    if (dir < 0) dir += 360;
    datum.vD = dir;
    datum.vV = vz;

    return 0;
}

int get_GPStime() {
    int i;
    ui8_t bytes[4];
    int val;

    for (i = 0; i < 3; i++)  bytes[i] = frame_bytes[pos_GPStime + i];
    val = 0;
    for (i = 0; i < 3; i++)  val |= bytes[i] << (8*(2-i));
    datum.time = val;

    return 0;
}

int get_GPSdate() {
    int i;
    ui8_t bytes[4];
    int val;

    for (i = 0; i < 3; i++)  bytes[i] = frame_bytes[pos_GPSdate + i];
    val = 0;
    for (i = 0; i < 3; i++)  val |= bytes[i] << (8*(2-i));
    datum.date = val;

    return 0;
}

/* -------------------------------------------------------------------------- */

int get_SN() {
    int i;
    unsigned byte;
    ui8_t sn_bytes[5];

    for (i = 0; i < 11; i++) datum.SN[i] = ' '; datum.SN[11] = '\0';

    for (i = 0; i < 5; i++) {
        byte = frame_bytes[pos_SN + i];
        sn_bytes[i] = byte;
    }

    byte = sn_bytes[2];
    sprintf(datum.SN, "%1X%02u", (byte>>4)&0xF, byte&0xF);
    byte = sn_bytes[3] | (sn_bytes[4]<<8);
    sprintf(datum.SN+3, " %1X %1u%04u", sn_bytes[0]&0xF, (byte>>13)&0x7, byte&0x1FFF);

    return 0;
}

/* -------------------------------------------------------------------------- */
/*
g : F^n -> F^16      // checksum, linear
g(m||b) = f(g(m),b)

// update checksum
f : F^16 x F^8 -> F^16 linear

010100001000000101000000
001010000100000010100000
000101000010000001010000
000010100001000000101000
000001010000100000010100
100000100000010000001010
000000011010100000000100
100000000101010000000010
000000001000000000000000
000000000100000000000000
000000000010000000000000
000000000001000000000000
000000000000100000000000
000000000000010000000000
000000000000001000000000
000000000000000100000000
*/

int update_checkM10(int c, ui8_t b) {
    int c0, c1, t, t6, t7, s;

    c1 = c & 0xFF;

    // B
    b  = (b >> 1) | ((b & 1) << 7);
    b ^= (b >> 2) & 0xFF;

    // A1
    t6 = ( c     & 1) ^ ((c>>2) & 1) ^ ((c>>4) & 1);
    t7 = ((c>>1) & 1) ^ ((c>>3) & 1) ^ ((c>>5) & 1);
    t = (c & 0x3F) | (t6 << 6) | (t7 << 7);

    // A2
    s  = (c >> 7) & 0xFF;
    s ^= (s >> 2) & 0xFF;


    c0 = b ^ t ^ s;

    return ((c1<<8) | c0) & 0xFFFF;
}

int checkM10(ui8_t *msg, int len) {
    int i, cs;

    cs = 0;
    for (i = 0; i < len; i++) {
        cs = update_checkM10(cs, msg[i]);
    }

    return cs & 0xFFFF;
}

/* -------------------------------------------------------------------------- */

int print_pos(int csOK) {
    int err;

    err = 0;
    err |= get_GPSpos();
    err |= get_GPSvel();
    err |= get_GPStime();
    err |= get_GPSdate();

    if (!err) {

        if (option_color) {
            fprintf(stdout, col_TXT);
            fprintf(stdout, " "col_GPSdate"%02d-%02d-%02d"col_TXT, datum.date/10000, (datum.date%10000)/100, datum.date%100);
            fprintf(stdout, " "col_GPStime"%02d:%02d:%02d "col_TXT, datum.time/10000, (datum.time%10000)/100, (datum.time%100)/1);

            fprintf(stdout, " lat: "col_GPSlat"%.6f"col_TXT"° ", datum.lat/1e6);
            fprintf(stdout, " lon: "col_GPSlon"%.6f"col_TXT"° ", datum.lon/1e6);
            fprintf(stdout, " alt: "col_GPSalt"%.2f"col_TXT"m ", datum.alt/1e2);

            //fprintf(stdout, "  (%.1f , %.1f , %.1f) ", datum.vE/1e2, datum.vN/1e2, datum.vU/1e2);
            fprintf(stdout, "  vH: "col_GPSvel"%.1f"col_TXT"m/s  D: "col_GPSvel"%.1f"col_TXT"°  vV: "col_GPSvel"%.1f"col_TXT"m/s", datum.vH, datum.vD, datum.vV);

            if (option_verbose >= 2) {
                get_SN();
                fprintf(stdout, "   SN: "col_SN"%s"col_TXT, datum.SN);
                fprintf(stdout, "  # ");
                if (csOK) fprintf(stdout, " "col_CSok"[OK]"col_TXT);
                else      fprintf(stdout, " "col_CSno"[NO]"col_TXT);
            }
            fprintf(stdout, ANSI_COLOR_RESET"");
        }
        else {
            fprintf(stdout, " %02d-%02d-%02d", datum.date/10000, (datum.date%10000)/100, datum.date%100);
            fprintf(stdout, " %02d:%02d:%02d ", datum.time/10000, (datum.time%10000)/100, (datum.time%100)/1);

            fprintf(stdout, " lat: %.6f° ", datum.lat/1e6);
            fprintf(stdout, " lon: %.6f° ", datum.lon/1e6);
            fprintf(stdout, " alt: %.2fm ", datum.alt/1e2);

            //fprintf(stdout, "  (%.1f , %.1f , %.1f) ", datum.vE/1e2, datum.vN/1e2, datum.vU/1e2);
            fprintf(stdout, "  vH: %.1fm/s  D: %.1f°  vV: %.1fm/s", datum.vH, datum.vD, datum.vV);

            if (option_verbose >= 2) {
                get_SN();
                fprintf(stdout, "   SN: %s", datum.SN);
                fprintf(stdout, "  # ");
                if (csOK) fprintf(stdout, " [OK]"); else fprintf(stdout, " [NO]");
            }
        }
    }
    fprintf(stdout, "\n");

    return err;
}

void print_frame(int pos) {
    int i;
    ui8_t byte;
    int cs1, cs2;
    int flen = stdFLEN; // stdFLEN=0x64, auxFLEN=0x76

    if (option_b < 2) {
        dpsk_bpm(frame_rawbits, frame_bits, RAWBITFRAME_LEN+RAWBITAUX_LEN);
    }
    bits2bytes(frame_bits, frame_bytes);
    flen = frame_bytes[0];
    if (flen == stdFLEN) auxlen = 0;
    else {
        auxlen = flen - stdFLEN;
        if (auxlen < 0 || auxlen > AUX_LEN) auxlen = 0;
    }

    cs1 = (frame_bytes[pos_Check] << 8) | frame_bytes[pos_Check+1];
    cs2 = checkM10(frame_bytes, pos_Check);

    if (option_raw) {

        if (option_color  &&  frame_bytes[1] != 0x49) {
            fprintf(stdout, col_FRTXT);
            for (i = 0; i < FRAME_LEN+auxlen; i++) {
                byte = frame_bytes[i];
                if ((i >= pos_GPSlat)   &&  (i < pos_GPSlat+4))   fprintf(stdout, col_GPSlat);
                if ((i >= pos_GPSlon)   &&  (i < pos_GPSlon+4))   fprintf(stdout, col_GPSlon);
                if ((i >= pos_GPSalt)   &&  (i < pos_GPSalt+3))   fprintf(stdout, col_GPSalt);
                if ((i >= pos_GPSvE)    &&  (i < pos_GPSvE+6))    fprintf(stdout, col_GPSvel);
                if ((i >= pos_GPStime)  &&  (i < pos_GPStime+3))  fprintf(stdout, col_GPStime);
                if ((i >= pos_GPSdate)  &&  (i < pos_GPSdate+3))  fprintf(stdout, col_GPSdate);
                if ((i >= pos_SN)       &&  (i < pos_SN+5))       fprintf(stdout, col_SN);
                if ((i >= pos_Check)    &&  (i < pos_Check+2))    fprintf(stdout, col_Check);
                fprintf(stdout, "%02x", byte);
                fprintf(stdout, col_FRTXT);
            }
            if (option_verbose) {
                fprintf(stdout, " # "col_Check"%04x"col_FRTXT, cs2);
                if (cs1 == cs2) fprintf(stdout, " "col_CSok"[OK]"col_TXT);
                else            fprintf(stdout, " "col_CSno"[NO]"col_TXT);
            }
            fprintf(stdout, ANSI_COLOR_RESET"\n");
        }
        else {
            for (i = 0; i < FRAME_LEN+auxlen; i++) {
                byte = frame_bytes[i];
                fprintf(stdout, "%02x", byte);
            }
            if (option_verbose) {
                fprintf(stdout, " # %04x", cs2);
                if (cs1 == cs2) fprintf(stdout, " [OK]"); else fprintf(stdout, " [NO]");
            }
            fprintf(stdout, "\n");
        }

    }
    else print_pos(cs1 == cs2);

}

/* -------------------------------------------------------------------------- */


int main(int argc, char **argv) {

    FILE *fp;
    char *fpname;
    int i, len;
    int bit, bit0;
    int pos;
    int header_found = 0;


#ifdef CYGWIN
    _setmode(fileno(stdin), _O_BINARY);  // _setmode(_fileno(stdin), _O_BINARY);
#endif
    setbuf(stdout, NULL);

    fpname = argv[0];
    ++argv;
    while ((*argv) && (!wavloaded)) {
        if      ( (strcmp(*argv, "-h") == 0) || (strcmp(*argv, "--help") == 0) ) {
            fprintf(stderr, "%s [options] audio.wav\n", fpname);
            fprintf(stderr, "  options:\n");
            //fprintf(stderr, "       -v, --verbose\n");
            fprintf(stderr, "       -r, --raw\n");
            fprintf(stderr, "       -c, --color\n");
            //fprintf(stderr, "       -o, --offset\n");
            return 0;
        }
        else if ( (strcmp(*argv, "-v") == 0) || (strcmp(*argv, "--verbose") == 0) ) {
            option_verbose = 1;
        }
        else if ( (strcmp(*argv, "-vv") == 0) ) option_verbose = 2;
        else if ( (strcmp(*argv, "-r") == 0) || (strcmp(*argv, "--raw") == 0) ) {
            option_raw = 1;
        }
        else if ( (strcmp(*argv, "-i") == 0) || (strcmp(*argv, "--invert") == 0) ) {
            option_inv = 1;  // nicht noetig
        }
        else if ( (strcmp(*argv, "-c") == 0) || (strcmp(*argv, "--color") == 0) ) {
            option_color = 1;
        }
        else if   (strcmp(*argv, "--res") == 0) { option_res = 1; }
        else if   (strcmp(*argv, "-b" ) == 0) { option_b = 1; }
        else if   (strcmp(*argv, "-b2") == 0) { option_b = 2; }
        else if   (strcmp(*argv, "--ch2") == 0) { wav_channel = 1; }  // right channel (default: 0=left)
        else {
            fp = fopen(*argv, "rb");
            if (fp == NULL) {
                fprintf(stderr, "%s konnte nicht geoeffnet werden\n", *argv);
                return -1;
            }
            wavloaded = 1;
        }
        ++argv;
    }
    if (!wavloaded) fp = stdin;


    i = read_wav_header(fp);
    if (i) {
        fclose(fp);
        return -1;
    }


    pos = FRAMESTART;

    while (!read_bits_fsk(fp, &bit, &len)) {

        if (len == 0) { // reset_frame();
            if (pos > (pos_GPSdate+4)*2*BITS) {
                for (i = pos; i < RAWBITFRAME_LEN+RAWBITAUX_LEN; i++) frame_rawbits[i] = 0x30 + 0;
                print_frame(pos);//byte_count
                header_found = 0;
                pos = FRAMESTART;
            }
            //inc_bufpos();
            //buf[bufpos] = 'x';
            continue;   // ...
        }

        for (i = 0; i < len; i++) {

            inc_bufpos();
            buf[bufpos] = 0x30 + bit;  // Ascii

            if (!header_found) {
                header_found = compare2();
            }
            else {
                frame_rawbits[pos] = 0x30 + bit;  // Ascii
                pos++;

                if (pos == RAWBITFRAME_LEN+RAWBITAUX_LEN) {
                    frame_rawbits[pos] = '\0';
                    print_frame(pos);//FRAME_LEN
                    header_found = 0;
                    pos = FRAMESTART;
                }
            }

        }
        if (header_found && option_b==1) {
            bitstart = 1;

            while ( pos < RAWBITFRAME_LEN+RAWBITAUX_LEN ) {
                if (read_rawbit(fp, &bit) == EOF) break;
                frame_rawbits[pos] = 0x30 + bit;
                pos++;
            }
            frame_rawbits[pos] = '\0';
            print_frame(pos);
            header_found = 0;
            pos = FRAMESTART;
        }
        if (header_found && option_b>=2) {
            bitstart = 1;
            bit0 = 0;

            if (pos%2) {
                if (read_rawbit(fp, &bit) == EOF) break;
                    frame_rawbits[pos] = 0x30 + bit;
                    pos++;
            }

            bit0 = dpsk_bpm(frame_rawbits, frame_bits, pos);
            pos /= 2;

            while ( pos < BITFRAME_LEN+BITAUX_LEN ) {
                if (read_rawbit2(fp, &bit) == EOF) break;
                frame_bits[pos] = 0x31 ^ (bit0 ^ bit);
                pos++;
                bit0 = bit;
            }
            frame_bits[pos] = '\0';
            print_frame(pos);
            header_found = 0;
            pos = FRAMESTART;
        }
    }

    fprintf(stdout, "\n");

    fclose(fp);

    return 0;
}

