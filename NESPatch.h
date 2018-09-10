#pragma once

/*
Copyright 2018 Clint Levijoki

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the 
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE 
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR 
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

class NESPatch : public Patch
{
	static inline float Clamp01(float f)
	{
		return f > 1 ? 1 : (f < 0 ? 0 : f);
	}

	static inline float Lerp(float f, float min, float max)
	{
		return min + (max-min) * Clamp01(f);
	}

	static inline float MapParam(float param, float min, float max)
	{
		const float buffer= 0.02f;
		// params seem to not go from 0 to 1 exactly, so remap them so we can get min and max values
		return Lerp((param-buffer)*(1/(1-buffer*2)), min, max);
	}

	static inline float Squaref(float x)
	{
		return x*x;
	}

	static inline uint32_t xorshift32(uint32_t *state)
	{
		/* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
		uint32_t x = *state;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		*state = x;
		return x;
	}

	// return -1 to 1
	float RandomUnit(uint32_t *state)
	{
		return (float(xorshift32(state) & 0xffff)/float(0x7fff)) - 1;
	}

	float Random01(uint32_t *state)
	{
		return (float(xorshift32(state) & 0xffff)/float(0xffff));
	}

	static const int DELAY_BUFFER_SIZE = (480-AUDIO_BLOCK_SIZE) & ~0x03; // four-element align
	static const int VOLUME_BUFFER_RESOLUTION = 4;
	static const int VOLUME_BUFFER_SIZE = (DELAY_BUFFER_SIZE*3)/VOLUME_BUFFER_RESOLUTION;

	float m_Buffer[DELAY_BUFFER_SIZE + AUDIO_BLOCK_SIZE+1] = {0};
	q7_t m_VolumeBuffer[(DELAY_BUFFER_SIZE*3 + AUDIO_BLOCK_SIZE+1)/VOLUME_BUFFER_RESOLUTION] = {0};
	int m_WriteIndex = 0;

	int m_SamplesLeft = 0;
	float m_Velocity = 0;
	float m_NextVelocity = 0;
	float m_Position = 0;
	float m_TargetPosition = 0;

	float m_TriangleParam = 0;
	float m_SawParam = 0;
	float m_AttackParam = 0;
	float m_CompressionParam = 0;

	uint32_t m_RandomState = 12345;
	float m_AttackStrength = 0;

	enum
	{
		TriangleParam,
		SawParam,
		Attack,
		Compression,
		ParamCount
	};

	void updateParams(const float* params)
	{
		m_TriangleParam = MapParam(params[TriangleParam], 0, 1);
		m_SawParam = MapParam(params[SawParam], 0.5f, 1);
		m_AttackParam = MapParam(params[Attack], 0, 2);
		m_CompressionParam = MapParam(params[Compression], 0, 1);
	}

	float preUpdate(const float value)
	{
		const float COMPRESSION_GAIN = 0.20f;
		const float compressed_value = (1-Squaref(Squaref(Squaref(Squaref(1 - std::abs(value)))))) * (value < 0 ? -COMPRESSION_GAIN : COMPRESSION_GAIN);
		return Lerp(m_CompressionParam, value, compressed_value);
	}

	float processSample(const float* values, q7_t *volume_values, bool update_volume)
	{
		if (m_SamplesLeft == 0)
		{
			const bool positive = values[0] > 0;
			float area = values[0];

			m_SamplesLeft = 1;
			while (m_SamplesLeft < DELAY_BUFFER_SIZE)
			{
				if ((values[m_SamplesLeft] > 0) != positive)
					break;

				area += values[m_SamplesLeft];

				m_SamplesLeft++;
			}

			m_TargetPosition = area / float(m_SamplesLeft);

			m_Position = Lerp(m_TriangleParam, m_TargetPosition, 0);

			const float use_saw_param = m_TargetPosition > m_Position ? m_SawParam : 1-m_SawParam;

			const float up_samples = std::max<float>(1, float(m_SamplesLeft) * use_saw_param);
			const float down_samples = std::max<float>(1, float(m_SamplesLeft) * (1-use_saw_param));

			m_Velocity = (m_TargetPosition-m_Position) / up_samples;
			m_NextVelocity = (m_Position-m_TargetPosition) / down_samples;
		}
		else
		{
			m_SamplesLeft--;

			m_Position += m_Velocity;

			if ((m_Velocity > 0 && m_Position >= m_TargetPosition) ||
				(m_Velocity < 0 && m_Position <= m_TargetPosition))
			{
				m_Velocity = m_NextVelocity;
				m_Position = m_TargetPosition;
				m_NextVelocity = 0;
				m_TargetPosition = 0;
			}
		}

		if (m_AttackParam > 0 && update_volume)
		{
			uint32_t tmp = 0;

			q7_t delay_max = 0;
			q7_t attack_max = 0;

			// shrinking this makes our delay buffer closer to the attack buffer, meaning
			// the attack will not lead as far
			const int USE_VOLUME_BUFFER_SIZE = VOLUME_BUFFER_SIZE-(AUDIO_BLOCK_SIZE*2)/VOLUME_BUFFER_RESOLUTION;

			const int ATTACK_WINDOW = USE_VOLUME_BUFFER_SIZE/6;
			const int DELAY_WINDOW = USE_VOLUME_BUFFER_SIZE-ATTACK_WINDOW;


			arm_max_q7(volume_values + USE_VOLUME_BUFFER_SIZE - (ATTACK_WINDOW+DELAY_WINDOW), DELAY_WINDOW, &delay_max, &tmp);
			arm_max_q7(volume_values + USE_VOLUME_BUFFER_SIZE - (ATTACK_WINDOW), ATTACK_WINDOW, &attack_max, &tmp);

			{
				q7_t q7_buf[] = { attack_max, delay_max };
				arm_max_q7(q7_buf, 2, &attack_max, &tmp);
			}

			float float_buf[2];
			{
				q7_t q7_buf[] = { attack_max, delay_max };
				arm_q7_to_float(q7_buf, float_buf, 2);			
			}

			m_AttackStrength = std::max(0.f, float_buf[0]-float_buf[1]);
		}

		const float attack_noise = RandomUnit(&m_RandomState) * m_AttackStrength;
		return Lerp(m_AttackStrength * m_AttackParam, m_Position, attack_noise);
	}

public:

	void processAudio(AudioBuffer &buffer) override
	{
		float params[ParamCount];
		static_assert(ParamCount <= 4, "Too many params");

		for (int n = 0; n < ParamCount; ++n)
		{
			params[n] = getParameterValue(PatchParameterId(n));
		}

		updateParams(params);

		const int size = buffer.getSize();

		float* buf_c0 = buffer.getSamples(0);

		// for ease of processing (not having to deal with the complexities of a ring buffer)
		// just shift our buffer back and copy the new samples at the end, so 
		// utility functions can find samples up to BUFFER_SIZE before the current samples
		arm_copy_f32(m_Buffer + size, m_Buffer, DELAY_BUFFER_SIZE);
		arm_copy_f32(buf_c0, m_Buffer + DELAY_BUFFER_SIZE, size);

		arm_copy_q7(m_VolumeBuffer + size/VOLUME_BUFFER_RESOLUTION, m_VolumeBuffer, VOLUME_BUFFER_SIZE);

		uint32_t tmp;
		q7_t q7_buf[VOLUME_BUFFER_RESOLUTION];

		for (int n = 0, m = VOLUME_BUFFER_SIZE; n < size; n += VOLUME_BUFFER_RESOLUTION, ++m)
		{
			arm_float_to_q7(buf_c0 + n, q7_buf, VOLUME_BUFFER_RESOLUTION);
			arm_abs_q7(q7_buf, q7_buf, VOLUME_BUFFER_RESOLUTION);

			// store the max value in our destination volume buffer
			arm_max_q7(q7_buf, VOLUME_BUFFER_RESOLUTION, m_VolumeBuffer + m, &tmp);
		}

		for (int n = 0; n < size; ++n)
		{
			buf_c0[n] = processSample(m_Buffer + n, m_VolumeBuffer + n/VOLUME_BUFFER_RESOLUTION, (n % VOLUME_BUFFER_RESOLUTION) == 0);
		}
	}	
};
