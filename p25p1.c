#include "dsd.h"
#include "p25p1_const.h"

static unsigned char mfid_mapping[64] = {
     0,  1,  2, 0,  3,  0,  4, 0,
     5,  0,  6, 0,  7,  8,  9, 0,
    10,  0, 11, 0, 12, 13, 14, 0,
    15, 16, 17, 0, 18, 19, 20, 21,
    22, 23,  0, 0, 24, 0,  0,  0,
    25, 26,  0, 0, 27, 0,  0,  0,
    28,  0, 29, 0, 30, 0, 31,  0,
    32,  0,  0, 0, 33, 0, 34, 35
};

static const char *mfids[36] = {
   "Standard (pre-2001)",
   "Standard (post-2001)",
   "Aselsan Inc.",
   "Relm / BK Radio",
   "EADS Public Safety Inc.",
   "Cycomm",
   "Efratom Time and Frequency Products, Inc",
   "Com-Net Ericsson",
   "Etherstack",
   "Datron",
   "Icom",
   "Garmin",
   "GTE",
   "IFR Systems",
   "INIT Innovations in Transportation, Inc",
   "GEC-Marconi",
   "Harris Corp.",
   "Kenwood Communications",
   "Glenayre Electronics",
   "Japan Radio Co.",
   "Kokusai",
   "Maxon",
   "Midland",
   "Daniels Electronics Ltd.",
   "Motorola",
   "Thales",
   "M/A-COM",
   "Raytheon",
   "SEA",
   "Securicor",
   "ADI",
   "Tait Electronics",
   "Teletec",
   "Transcrypt International",
   "Vertex Standard",
   "Zetron, Inc"
};

void
read_dibit (dsd_opts* opts, dsd_state* state, unsigned char* output, unsigned int count, int* status_count)
{
    unsigned int i;

    for (i = 0; i < count; i++) {
        if (*status_count == 35) {
            // Status bits now
            // TODO: do something useful with the status bits...
            //int status = getDibit (opts, state);
            getDibit (opts, state);
            *status_count = 1;
        } else {
            (*status_count)++;
        }

        output[i] = getDibit(opts, state);
    }
}

void
skip_dibit(dsd_opts* opts, dsd_state* state, unsigned int count, int* status_count)
{
    unsigned int i;

    for (i = 0; i < count; i++) {
        if (*status_count == 35) {
            // Status bits now
            // TODO: do something useful with the status bits...
            //int status = getDibit (opts, state);
            getDibit (opts, state);
            *status_count = 1;
        } else {
            (*status_count)++;
        }

        getDibit(opts, state);
    }
}

static void read_IMBE (dsd_opts* opts, dsd_state* state, int* status_count)
{
  unsigned int i, dibit;
  unsigned char imbe_dibits[72];
  char imbe_fr[8][23];

  read_dibit(opts, state, imbe_dibits, 72, status_count);

  for (i = 0; i < 72; i++) {
      dibit = imbe_dibits[i];
      imbe_fr[iW[i]][iX[i]] = (1 & (dibit >> 1)); // bit 1
      imbe_fr[iY[i]][iZ[i]] = (1 & dibit);        // bit 0
  }

  process_IMBE(opts, state, imbe_fr);
}

/**
 * Corrects a hex (12 bit) word using the Golay 23 FEC.
 */
static unsigned int
correct_hex_word (dsd_state* state, unsigned char *hex_and_parity, unsigned int codeword_bits)
{
  unsigned int i, golay_codeword = 0, golay_codeword_old = 0, fixed_errors = 0;

  // codeword now contains:
  // bits 0-10: golay (23,12) parity bits
  // bits 11-22: hex bits

  for (i = 0; i < codeword_bits; i++) {
    golay_codeword <<= 2;
    golay_codeword |= hex_and_parity[i];
  }
  golay_codeword >>= 1;
  golay_codeword_old = (golay_codeword >> 11);
  Golay23_Correct(&golay_codeword);

  if (golay_codeword_old != golay_codeword) {
    fixed_errors++;
  }

  state->debug_header_errors += fixed_errors;
  return golay_codeword;
}

void update_p25_error_stats (dsd_state* state, unsigned int nbits, unsigned int n_errs)
{
    state->p25_bit_count += nbits;
    state->p25_bit_error_count += n_errs;
    while ((!(state->p25_bit_count & 1)) && (!(state->p25_bit_error_count & 1))) {
        state->p25_bit_count >>= 1;
        state->p25_bit_error_count >>= 1;
    }
}

float get_p25_ber_estimate (dsd_state* state)
{
    float ber = 0.0f;
    if (state->p25_bit_count > 0) {
        ber = (((float)state->p25_bit_count) * 100.0f / ((float)state->p25_bit_error_count));
    }
    return ber;
}

/**
 * The important method that processes a full P25 HD unit.
 */
static unsigned int 
processHDU(dsd_opts* opts, dsd_state* state)
{
  int i;
  unsigned char mfid = 0;
  unsigned char algid = 0;
  unsigned int talkgroup = 0;
  int status_count;
  unsigned char hex_and_parity[18];
  unsigned char hex_data[20];    // Data in hex-words (6 bit words). A total of 20 hex words.

  // we skip the status dibits that occur every 36 symbols
  // the next status symbol comes in 14 dibits from here
  // so we start counter at 36-14-1 = 21
  status_count = 21;

  // Read 20 hex words, correct them using their Golay23 parity data.
  for (i = 0; i < 20; i++) {
      // read both the hex word and the Golay(23, 12) parity information
      read_dibit(opts, state, hex_and_parity, 9, &status_count);
      // Use the Golay23 FEC to correct it.
      hex_data[i] = correct_hex_word(state, hex_and_parity, 9);
  }

  // Skip the 16 parity hex word.
  skip_dibit(opts, state, 9*16, &status_count);

  // Now put the corrected data on the DSD structures
  mfid = ((hex_data[12] << 2) | (hex_data[13] & 3));

  // The important algorithm ID. This indicates whether the data is encrypted,
  // and if so what is the encryption algorithm used.
  // A code 0x80 here means that the data is unencrypted.
  algid  = (((hex_data[13] >> 2) << 4) | (hex_data[14] & 0x0f));
  state->p25enc = (algid != 0x80);

  skipDibit (opts, state, 6);

  if ((mfid == 144) || (mfid == 9)) { // FIXME: only one of these is correct.
    talkgroup  = ((hex_data[18] << 6) | hex_data[19]);
  } else {
    talkgroup  = (((hex_data[17] >> 2) << 12) | (hex_data[18] << 6) | (hex_data[19]));
  }

  state->talkgroup = talkgroup;
  return mfid;
}

static unsigned int
read_and_correct_hex_word (dsd_opts* opts, dsd_state* state, int* status_count)
{
  unsigned char hex_and_parity[40];
  unsigned int i, value_in[4], value = 0, value_out = 0;

  // Read the hexword and parity
  read_dibit(opts, state, hex_and_parity, 20, status_count);

  // Use Hamming to error correct the hex word
  // in the bitset 9 is the left-most and 0 is the right-most
  value_in[3] = value_in[2] = value_in[1] = value_in[0] = 0;  
  for (i=0; i<5; i++) {
      value_in[0] <<= 2;
      value_in[0] |= hex_and_parity[i];
      value_in[1] <<= 2;
      value_in[1] |= hex_and_parity[i+5];
      value_in[2] <<= 2;
      value_in[2] |= hex_and_parity[i+10];
      value_in[3] <<= 2;
      value_in[3] |= hex_and_parity[i+15];
  }
  value = value_in[0];
  p25_Hamming10_6_4_Correct(&value);
  value_in[0] >>= 4;
  if (value != value_in[0]) { state->debug_header_errors++; }
  value_out <<= 6;
  value_out |= value;

  value = value_in[1];
  p25_Hamming10_6_4_Correct(&value);
  value_in[1] >>= 4;
  if (value != value_in[1]) { state->debug_header_errors++; }
  value_out <<= 6;
  value_out |= value;

  value = value_in[2];
  p25_Hamming10_6_4_Correct(&value);
  value_in[2] >>= 4;
  if (value != value_in[2]) { state->debug_header_errors++; }
  value_out <<= 6;
  value_out |= value;

  value = value_in[3];
  p25_Hamming10_6_4_Correct(&value);
  value_in[3] >>= 4;
  if (value != value_in[3]) { state->debug_header_errors++; }
  value_out <<= 6;
  value_out |= value;

  return value_out;
}

static void
processLDU1 (dsd_opts* opts, dsd_state* state, char *outstr, unsigned int outlen)
{
  // extracts IMBE frames from LDU frame
  unsigned int i, lsd1 = 0, lsd2 = 0;
  unsigned int lcinfo[3];    // Data in hex-words (6 bit words), stored packed in groups of four, in a uint32_t
  unsigned char lsd[16];
  unsigned char mfid;
  int status_count;

  // we skip the status dibits that occur every 36 symbols
  // the first IMBE frame starts 14 symbols before next status
  // so we start counter at 36-14-1 = 21
  status_count = 21;

  // IMBE 1
  read_IMBE (opts, state, &status_count);

  // IMBE 2
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 2
  lcinfo[0] = read_and_correct_hex_word (opts, state, &status_count);

  // IMBE 3
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 3
  lcinfo[1] = read_and_correct_hex_word (opts, state, &status_count);

  // IMBE 4
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 4
  lcinfo[2] = read_and_correct_hex_word (opts, state, &status_count);

  // IMBE 5
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 5
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 6
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 6
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 7
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 7
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 8
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 8: LSD (low speed data)
  read_dibit(opts, state, lsd, 16, &status_count);

  for (i=0; i<8; i++) {
    lsd1 <<= 2;
    lsd1 |= lsd[i];
    lsd2 <<= 2;
    lsd2 |= lsd[i+8];
  }

  p25_lsd_cyclic1685_Correct(&lsd1);
  p25_lsd_cyclic1685_Correct(&lsd2);
  // TODO: do something useful with the LSD bytes...

  // IMBE 9
  read_IMBE (opts, state, &status_count);

  // trailing status symbol
  getDibit (opts, state);

  state->talkgroup = (lcinfo[1] & 0xFFFF);
  state->radio_id = lcinfo[2];

  mfid  = ((lcinfo[0] >> 10) & 0xFF);
  snprintf(outstr, outlen, "LDU1: e: %u, mfid: %s (%u), talkgroup: %u, src: %u, lsd: 0x%02x/0x%02x",
          state->errs2, mfids[mfid_mapping[mfid&0x3f]], mfid, state->talkgroup, state->radio_id, lsd1, lsd2);
  //decode_p25_lcf(lcinfo);
}

static unsigned int 
processLDU2 (dsd_opts * opts, dsd_state * state)
{
  // extracts IMBE frames from LDU frame
  unsigned char lsd[16];
  unsigned int i, lsd1 = 0, lsd2 = 0;
  int status_count;

  // we skip the status dibits that occur every 36 symbols
  // the first IMBE frame starts 14 symbols before next status
  // so we start counter at 36-14-1 = 21
  status_count = 21;

  // IMBE 1
  read_IMBE (opts, state, &status_count);

  // IMBE 2
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 2
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 3
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 3
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 4
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 4
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 5
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 5
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 6
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 6
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 7
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 7
  skip_dibit (opts, state, 20, &status_count);

  // IMBE 8
  read_IMBE (opts, state, &status_count);

  // Read data after IMBE 8: LSD (low speed data)
  read_dibit(opts, state, lsd, 16, &status_count);

  for (i=0; i<8; i++) {
    lsd1 <<= 2;
    lsd1 |= lsd[i];
    lsd2 <<= 2;
    lsd2 |= lsd[i+8];
  }

  p25_lsd_cyclic1685_Correct(&lsd1);
  p25_lsd_cyclic1685_Correct(&lsd2);
  //state->p25lsd1 = lsd1;
  //state->p25lsd2 = lsd2;
  // TODO: do something useful with the LSD bytes...

  // IMBE 9
  read_IMBE (opts, state, &status_count);

  // trailing status symbol
  getDibit (opts, state);

  return state->errs2;
}

static void
processTDU (dsd_opts* opts, dsd_state* state)
{
    // we skip the status dibits that occur every 36 symbols
    // the first IMBE frame starts 14 symbols before next status
    // so we start counter at 36-14-1 = 21

    // Next 14 dibits should be zeros
    skipDibit(opts, state, 14);

    // Next we should find an status dibit
    getDibit (opts, state);
}

static void
processTDULC (dsd_opts* opts, dsd_state* state, char *outstr, unsigned int outlen)
{
  unsigned int j, corrected_hexword = 0;
  unsigned char hex_and_parity[12];
  unsigned int lcinfo[3];
  unsigned int dodeca_data[6];    // Data in 12-bit words. A total of 6 words.
  unsigned char mfid;
  int status_count;

  // we skip the status dibits that occur every 36 symbols
  // the first IMBE frame starts 14 symbols before next status
  // so we start counter at 36-14-1 = 21
  status_count = 21;

  for (j = 0; j < 6; j++) {
      // read both the hex word and the Golay(23, 12) parity information
      read_dibit(opts, state, hex_and_parity, 12, &status_count);

      // Use the Golay23 FEC to correct it.
      corrected_hexword = correct_hex_word(state, hex_and_parity, 12);

      // codeword is now hopefully fixed
      // put it back into our hex format
      dodeca_data[5-j] = corrected_hexword;
  }

  skip_dibit(opts, state, 72, &status_count);

  // Next 10 dibits should be zeros
  skip_dibit(opts, state, 10, &status_count);

  // trailing status symbol
  getDibit (opts, state);

  // Put the corrected data into the DSD structures
  lcinfo[0]  = (dodeca_data[5]);
  lcinfo[0] |= (dodeca_data[4] << 12);
  lcinfo[1]  = (dodeca_data[3]);
  lcinfo[1] |= (dodeca_data[2] << 12);
  lcinfo[2]  = (dodeca_data[1]);
  lcinfo[2] |= (dodeca_data[0] << 12);

  mfid  = ((lcinfo[0] >> 10) & 0xFF);
  snprintf(outstr, outlen, "TDULC: mfid: %s (%u)", mfids[mfid_mapping[mfid&0x3f]], mfid);
  //decode_p25_lcf(lcinfo);
}

/* Symbol interleaving, derived from CAI specification table 7.4
 */
static const size_t INTERLEAVING[] = {
    0, 13, 25, 37, 
    1, 14, 26, 38, 
    2, 15, 27, 39, 
    3, 16, 28, 40, 
    4, 17, 29, 41, 
    5, 18, 30, 42, 
    6, 19, 31, 43, 
    7, 20, 32, 44, 
    8, 21, 33, 45, 
    9, 22, 34, 46, 
   10, 23, 35, 47, 
   11, 24, 36, 48, 
   12 
};

typedef struct _Channel {
    uint8_t valid, tdma;
    uint16_t bandwidth, step;
    float freq;
} Channel;
static Channel p25_default_channels[17];

void p25_add_channel(uint8_t chan_id, unsigned int freq, uint16_t step, uint16_t bw_hz, uint8_t is_tdma) {
    printf("Add: iden: %u, freq %.5fkHz+%uHz, bandwidth %uHz / step %uHz, slots/carrier: %u\n",
           chan_id, 1000.0f * 5.0f * freq, 0, 10*bw_hz, step, is_tdma);
	p25_default_channels[chan_id & 0x0f].freq = 5.0f * (float)freq;
	p25_default_channels[chan_id & 0x0f].bandwidth = bw_hz;
	p25_default_channels[chan_id & 0x0f].step = step;
	p25_default_channels[chan_id & 0x0f].tdma = is_tdma;
	p25_default_channels[chan_id & 0x0f].valid = 1;
}

float p25_chid_to_freq(uint8_t chan_id, uint16_t channel) {
    if (p25_default_channels[chan_id & 0x0f].valid) {
		Channel *temp_chan = &p25_default_channels[chan_id];
        float freq = temp_chan->freq;
		if (temp_chan->tdma == 0) {
			return freq + temp_chan->step * channel;
		} else {
			return freq + temp_chan->step * (channel >> temp_chan->tdma);
		}
	}
	return  0;
}

static void
processTSBK(unsigned char raw_dibits[98], unsigned char out[12], int *status_count)
{
  unsigned char trellis_buffer[49];
  unsigned int i, opcode = 0, err = 0;

  for (i = 0; i < 49; i++) {
    unsigned int k = INTERLEAVING[i];
    unsigned char t = ((raw_dibits[2*k] << 2) | (raw_dibits[2*k+1] << 0));
    trellis_buffer[i] = t;
  }

  for (i = 0; i < 49; i++) {
    raw_dibits[49] = 0;
  }

  err = p25_trellis_1_2_decode(trellis_buffer, 49, raw_dibits); /* raw_dibits actually has decoded dibits here! */

  for(i = 0; i < 12; ++i) {
    out[i] = ((raw_dibits[4*i] << 6) | (raw_dibits[4*i+1] << 4) | (raw_dibits[4*i+2] << 2) | (raw_dibits[4*i+3] << 0));
  }

  opcode = (out[0] & 0x3f);
  if (err) {
    printf("TSBK: mfid: 0x%02x, lb: %u, opcode: 0x%02x, err: trellis decode failed, offset: %u\n",
           out[1], (out[0] >> 7), opcode, err);
  } else {
	unsigned int chan = (((out[7] << 8) | (out[8] << 0)) & 0x0fff);
	unsigned char chan_id = (out[7] >> 4);
	float f1 = p25_chid_to_freq(chan_id, chan);
    printf("TSBK: mfid: 0x%02x, lb: %u, opcode: 0x%02x\n", out[1], (out[0] >> 7), opcode);
	if (opcode == 0x16) { // sndcp_data_ch
        unsigned short ch1 = ((out[3] << 8) | out[4]);
        unsigned short ch2 = ((out[5] << 8) | out[6]);
	    printf("sndcp_data_ch: Ch1: %u -> Ch2: %u\n", ch1, ch2);
	} else if (opcode == 0x34) {  // iden_up vhf uhf
        unsigned char iden = (out[2] >> 4);
        unsigned short bwvu = (625 << (out[2] & 0x01));
        unsigned int step = 125*(((out[4] << 8) | out[5]) & 0x3ff);
        unsigned int freq = ((out[6] << 24) | (out[7] << 16) | (out[8] << 8) | (out[9] << 0)); 
        signed int toff = (((out[3] << 6) | (out[4] >> 2)) & 0x1fff);
		unsigned char toff_sign = (out[3] & 0x80);
		if (toff_sign == 0) {
			toff = 0 - toff;
		}
		p25_add_channel(iden, freq, step, bwvu, 0);
		printf("iden_up_vhf/uhf iden: %u, toff: %.5f, spacing: %u, freq: %.5fkHz [mob Tx%c]\n",
               iden, (toff * step * 10e-3f), step, (5.0f * freq * 10e-3f), (toff_sign ? '+' : '-'));
	} else if (opcode == 0x33) {  // iden_up_tdma
        unsigned char iden = (out[2] >> 4);
		unsigned char slots_per_carrier[] = {1,1,1,2,4,2};
		unsigned char channel_type = (out[3] & 0x0f);
        unsigned int step = 125*(((out[4] << 8) | out[5]) & 0x3ff);
        unsigned int freq = ((out[6] << 24) | (out[7] << 16) | (out[8] << 8) | (out[9] << 0)); 
        signed int toff = (((out[3] << 6) | (out[4] >> 2)) & 0x1fff);
		unsigned char toff_sign = (out[3] & 0x80);
		if (toff_sign == 0) {
			toff = 0 - toff;
		}
		p25_add_channel(iden, freq, step, 625, slots_per_carrier[channel_type]);
		printf("iden_up_tdma: iden: %u: freq: %.5fkHz, offset: %u, step: %u, slots/carrier: %u\n",
               iden, (5.0f * freq * 10e-3f), (toff * step), step, slots_per_carrier[channel_type]);
	} else if (opcode == 0x3d)  {  // iden_up
        unsigned char iden = (out[2] >> 4);
        unsigned short bw = (((out[2] << 5) | (out[3] >> 3)) & 0x01ff);
        unsigned int step = 125*(((out[4] << 8) | out[5]) & 0x3ff);
        unsigned int freq = ((out[6] << 24) | (out[7] << 16) | (out[8] << 8) | (out[9] << 0)); 
        signed short toff = (((out[3] << 6) | (out[4] >> 2)) & 0xff);
		unsigned char toff_sign = (out[3] & 0x04);
		if (toff_sign == 0) {
			toff = 0 - toff;
		}
		p25_add_channel(iden, freq, step, bw, 0);
		printf("iden_up: iden: %u, toff: %.5f, spacing: %u, freq: %.5fkHz [mob Tx%c]\n",
               iden, (toff * 0.25f), step, (5.0f * freq * 10e-3f), (toff_sign ? '+' : '-'));
    } else if (opcode == 0x3a) { // rfss_status
		unsigned short syid = (((out[3] & 0x0f) << 8) | out[4]);
		printf("rfss_status: LRA: %u, syid: %u, rfid: %u, siteid: %u, Ch1: iden: %u, ch: %u (%.5fMHz)\n",
               out[2], syid, out[5], out[6], chan_id, chan, (f1 * 10e-6f));
    } else if (opcode == 0x39) { // secondary CC
		unsigned int ch1 = ((out[4] << 8) | (out[5] << 0));
		printf("secondary cc: rfid %u, siteid: %u, Ch1: iden: %u, ch: %u -> Ch2: iden: %u, ch: %u\n",
               out[2], out[3], (ch1 >> 12), (ch1 & 0x0fff), chan_id, chan);
	} else if (opcode == 0x3b) {  // network status
		unsigned short wacn = ((out[3] << 12) | (out[4] << 4) | (out[5] >> 4));
		unsigned short syid = (((out[5] & 0x0f) << 8) | out[6]);
		printf("Network Status: LRA: %u, WACN: 0x%04x, syid: %u, Ch1: iden: %u, ch: %u (%.5fMHz)\n",
               out[2], wacn, syid, chan_id, chan, (f1 * 10e-6f));
    } else if (opcode == 0x3c) { // adjacent_status
		unsigned short syid = (((out[3] & 0x0f) << 8) | out[4]);
		printf("Adjacent Status: LRA: %u, syid: %u, rfid: %u, siteid: %u, Ch1: iden: %u, ch: %u (%.5fMHz)\n",
               out[2], syid, out[5], out[6], chan_id, chan, (f1 * 10e-6f));
    }
  }
}

static void
processTSDU(dsd_opts* opts, dsd_state* state)
{
  unsigned char last_block = 0, k = 0;
  unsigned char raw_dibits[98];
  unsigned char out[12];
  int status_count;

  // we skip the status dibits that occur every 36 symbols
  // the next status symbol comes in 14 dibits from here
  // so we start counter at 36-14-1 = 21
  status_count = 21;

  while (!last_block && (k < 3)) {
    read_dibit(opts, state, raw_dibits, 98, &status_count);
    processTSBK(raw_dibits, out, &status_count);
    last_block = (out[0] >> 7);
    k++;
  }

  // trailing status symbol
  getDibit (opts, state);
}

void process_p25_frame(dsd_opts *opts, dsd_state *state, char *tmpStr, unsigned int tmpLen)
{
  unsigned char duid = state->duid;
  unsigned int mfid = 0, errs2 = 0;

  if ((duid == 5) || ((duid == 10) && (state->lastp25type == 1))) {
      if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_fd == -1)) {
          state->errs2 = 0;
          openMbeOutFile (opts, state);
      }
  }

  if (duid == 0) {
      // Header Data Unit
      state->lastp25type = 2;
      mfid = processHDU (opts, state);
      snprintf(tmpStr, 1023, "HDU: mfid: %s (%u), talkgroup: %u",
               mfids[mfid_mapping[(mfid>>2)&0x3f]], mfid, state->talkgroup);
  } else if (duid == 5) { // 11 -> 0101 = 5
      // Logical Link Data Unit 1
      state->lastp25type = 1;
      processLDU1 (opts, state, tmpStr, 1023);
  } else if (duid == 10) { // 22 -> 1010 = 10
      // Logical Link Data Unit 2
      if (state->lastp25type != 1) {
          snprintf(tmpStr, 1023, "Ignoring LDU2 not preceeded by LDU1");
          state->lastp25type = 0;
      } else {
          state->lastp25type = 2;
          errs2 = processLDU2 (opts, state);
          snprintf(tmpStr, 1023, "LDU2: e: %u", errs2);
      }
  } else if ((duid == 3) || (duid == 15)) {
      if (opts->mbe_out_dir[0] != 0) {
          closeMbeOutFile (opts, state);
          state->errs2 = 0;
      }
      mbe_initMbeParms (&state->cur_mp, &state->prev_mp, &state->prev_mp_enhanced);
      state->talkgroup = 0;
      state->lastp25type = 0;
      if (duid == 3) {
        // Terminator without subsequent Link Control
        processTDU (opts, state);
        strcpy(tmpStr, "TDU");
      } else {
        // Terminator with subsequent Link Control
        processTDULC (opts, state, tmpStr, 1023);
      }
  } else if (duid == 7) { // 13 -> 0111 = 7
      state->talkgroup = 0;
      state->lastp25type = 3;
      processTSDU (opts, state);
      snprintf(tmpStr, 1023, "TSDU");
  // try to guess based on previous frame if unknown type
  } else if (state->lastp25type == 1) { 
      if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_fd == -1)) {
          openMbeOutFile (opts, state);
      }
      //state->lastp25type = 0;
      // Guess that the state is LDU2
      state->lastp25type = 2;
      errs2 = processLDU2 (opts, state);
      snprintf(tmpStr, 1023, "(LDU2): e: %u", errs2);
  } else if (state->lastp25type == 2) {
      if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_fd == -1)) {
          openMbeOutFile (opts, state);
      }
      //state->lastp25type = 0;
      // Guess that the state is LDU1
      state->lastp25type = 1;
      //printf("(LDU1)\n");
      processLDU1 (opts, state, tmpStr, 1023);
  } else if (state->lastp25type == 3) {
      //state->lastp25type = 0;
      // Guess that the state is TSDU
      state->lastp25type = 3;
      processTSDU (opts, state);
      //skipDibit (opts, state, 328-25);
      snprintf(tmpStr, 1023, "(TSDU)");
  } else { 
      state->lastp25type = 0;
      snprintf(tmpStr, 1023, "Unknown DUID");
  }
}

