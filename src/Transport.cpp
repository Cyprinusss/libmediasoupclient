#define MSC_CLASS "Transport"

#include "Transport.hpp"
#include "Logger.hpp"
#include "MediaSoupClientErrors.hpp"
#include "ortc.hpp"

using json = nlohmann::json;

namespace mediasoupclient
{
	/* Transport */

	Transport::Transport(
	  Listener* listener,
	  const std::string& id,
	  const nlohmann::json* extendedRtpCapabilities,
	  const nlohmann::json& appData)
	  : extendedRtpCapabilities(extendedRtpCapabilities), listener(listener), id(id), appData(appData)
	{
		MSC_TRACE();
	}

	const std::string& Transport::GetId() const
	{
		MSC_TRACE();

		return this->id;
	}

	bool Transport::IsClosed() const
	{
		MSC_TRACE();

		return this->closed;
	}

	const std::string& Transport::GetConnectionState() const
	{
		MSC_TRACE();

		return PeerConnection::iceConnectionState2String[this->connectionState];
	}

	nlohmann::json& Transport::GetAppData()
	{
		MSC_TRACE();

		return this->appData;
	}

	void Transport::Close()
	{
		MSC_TRACE();

		if (this->closed)
			return;

		this->closed = true;

		// Close the handler.
		this->handler->Close();
	}

	nlohmann::json Transport::GetStats() const
	{
		MSC_TRACE();

		if (this->closed)
			MSC_THROW_INVALID_STATE_ERROR("Transport closed");
		else
			return this->handler->GetTransportStats();
	}

	void Transport::RestartIce(const nlohmann::json& iceParameters)
	{
		MSC_TRACE();

		if (this->closed)
			MSC_THROW_INVALID_STATE_ERROR("Transport closed");
		else
			return this->handler->RestartIce(iceParameters);
	}

	void Transport::UpdateIceServers(const nlohmann::json& iceServers)
	{
		MSC_TRACE();

		if (this->closed)
			MSC_THROW_INVALID_STATE_ERROR("Transport closed");
		else
			return this->handler->UpdateIceServers(iceServers);
	}

	void Transport::SetHandler(Handler* handler)
	{
		MSC_TRACE();

		this->handler = handler;
	}

	void Transport::OnConnect(json& dtlsParameters)
	{
		MSC_TRACE();

		if (this->closed)
			MSC_THROW_INVALID_STATE_ERROR("Transport closed");

		return this->listener->OnConnect(this, dtlsParameters).get();
	}

	void Transport::OnConnectionStateChange(
	  webrtc::PeerConnectionInterface::IceConnectionState connectionState)
	{
		MSC_TRACE();

		// Update connection state.
		this->connectionState = connectionState;

		return this->listener->OnConnectionStateChange(
		  this, PeerConnection::iceConnectionState2String[connectionState]);
	}

	/* SendTransport */

	SendTransport::SendTransport(
	  Listener* listener,
	  const std::string& id,
	  const json& iceParameters,
	  const json& iceCandidates,
	  const json& dtlsParameters,
	  const json& sctpParameters,
	  const PeerConnection::Options* peerConnectionOptions,
	  const json* extendedRtpCapabilities,
	  const std::map<std::string, bool>* canProduceByKind,
	  const json& appData)

	  : Transport(listener, id, extendedRtpCapabilities, appData), listener(listener),
	    canProduceByKind(canProduceByKind)
	{
		MSC_TRACE();

		json sendingRtpParametersByKind = {
			{ "audio", ortc::getSendingRtpParameters("audio", *extendedRtpCapabilities) },
			{ "video", ortc::getSendingRtpParameters("video", *extendedRtpCapabilities) }
		};

		json sendingRemoteRtpParametersByKind = {
			{ "audio", ortc::getSendingRemoteRtpParameters("audio", *extendedRtpCapabilities) },
			{ "video", ortc::getSendingRemoteRtpParameters("video", *extendedRtpCapabilities) }
		};

		this->handler.reset(new SendHandler(
		  this,
		  iceParameters,
		  iceCandidates,
		  dtlsParameters,
		  sctpParameters,
		  peerConnectionOptions,
		  sendingRtpParametersByKind,
		  sendingRemoteRtpParametersByKind));

		Transport::SetHandler(this->handler.get());
	}

	/*
	 * Produce a track.
	 */
	Producer* SendTransport::Produce(
	  Producer::Listener* producerListener,
	  webrtc::MediaStreamTrackInterface* track,
	  const std::vector<webrtc::RtpEncodingParameters>* encodings,
	  const json* codecOptions,
	  json appData)
	{
		MSC_TRACE();

		if (this->closed)
			MSC_THROW_INVALID_STATE_ERROR("SendTransport closed");
		else if (!track)
			MSC_THROW_TYPE_ERROR("missing track");
		else if (track->state() == webrtc::MediaStreamTrackInterface::TrackState::kEnded)
			MSC_THROW_INVALID_STATE_ERROR("track ended");
		else if (this->canProduceByKind->find(track->kind()) == this->canProduceByKind->end())
			MSC_THROW_UNSUPPORTED_ERROR("cannot produce track kind");

		std::string producerId;

		std::vector<webrtc::RtpEncodingParameters> normalizedEncodings;

		if (encodings)
		{
			std::for_each(
			  encodings->begin(),
			  encodings->end(),
			  [&normalizedEncodings](const webrtc::RtpEncodingParameters& entry) {
				  webrtc::RtpEncodingParameters encoding;

				  encoding.active                   = entry.active;
				  encoding.dtx                      = entry.dtx;
				  encoding.max_bitrate_bps          = entry.max_bitrate_bps;
				  encoding.max_framerate            = entry.max_framerate;
				  encoding.scale_framerate_down_by  = entry.scale_framerate_down_by;
				  encoding.scale_resolution_down_by = entry.scale_resolution_down_by;

				  normalizedEncodings.push_back(encoding);
			  });
		}

		// May throw.
		auto result = this->handler->Send(track, &normalizedEncodings, codecOptions);

		auto& localId       = result.first;
		auto& rtpParameters = result.second;

		try
		{
			// This will fill rtpParameters's missing fields with default values.
			ortc::validateRtpParameters(rtpParameters);

			// May throw.
			producerId = this->listener->OnProduce(this, track->kind(), rtpParameters, appData).get();
		}
		catch (MediaSoupClientError& error)
		{
			this->handler->StopSending(localId);

			throw;
		}

		auto* producer =
		  new Producer(this, producerListener, producerId, localId, track, rtpParameters, appData);

		this->producers[producer->GetId()] = producer;

		return producer;
	}

	void SendTransport::Close()
	{
		MSC_TRACE();

		if (this->closed)
			return;

		Transport::Close();

		// Close all Producers.
		for (auto& kv : this->producers)
		{
			auto* producer = kv.second;

			producer->TransportClosed();
		}
	}

	void SendTransport::OnClose(Producer* producer)
	{
		MSC_TRACE();

		this->producers.erase(producer->GetId());

		if (this->closed)
			return;

		// May throw.
		this->handler->StopSending(producer->GetLocalId());
	}

	void SendTransport::OnReplaceTrack(const Producer* producer, webrtc::MediaStreamTrackInterface* track)
	{
		MSC_TRACE();

		return this->handler->ReplaceTrack(producer->GetLocalId(), track);
	}

	void SendTransport::OnSetMaxSpatialLayer(const Producer* producer, uint8_t maxSpatialLayer)
	{
		MSC_TRACE();

		return this->handler->SetMaxSpatialLayer(producer->GetLocalId(), maxSpatialLayer);
	}

	json SendTransport::OnGetStats(const Producer* producer)
	{
		MSC_TRACE();

		if (this->closed)
			MSC_THROW_INVALID_STATE_ERROR("SendTransport closed");

		return this->handler->GetSenderStats(producer->GetLocalId());
	}

	/* RecvTransport */

	RecvTransport::RecvTransport(
	  Listener* listener,
	  const std::string& id,
	  const json& iceParameters,
	  const json& iceCandidates,
	  const json& dtlsParameters,
	  const json& sctpParameters,
	  const PeerConnection::Options* peerConnectionOptions,
	  const json* extendedRtpCapabilities,
	  const json& appData)
	  : Transport(listener, id, extendedRtpCapabilities, appData)
	{
		MSC_TRACE();

		this->handler.reset(new RecvHandler(
		  this, iceParameters, iceCandidates, dtlsParameters, sctpParameters, peerConnectionOptions));

		Transport::SetHandler(this->handler.get());
	}

	/*
	 * Consume a remote Producer.
	 */
	Consumer* RecvTransport::Consume(
	  Consumer::Listener* consumerListener,
	  const std::string& id,
	  const std::string& producerId,
	  const std::string& kind,
	  json* rtpParameters,
	  json appData)
	{
		MSC_TRACE();

		if (this->closed)
			MSC_THROW_INVALID_STATE_ERROR("RecvTransport closed");
		else if (id.empty())
			MSC_THROW_TYPE_ERROR("missing id");
		else if (producerId.empty())
			MSC_THROW_TYPE_ERROR("missing producerId");
		else if (kind != "audio" && kind != "video")
			MSC_THROW_TYPE_ERROR("invalid kind");
		else if (!rtpParameters)
			MSC_THROW_TYPE_ERROR("missing rtpParameters");
		else if (!appData.is_object())
			MSC_THROW_TYPE_ERROR("appData must be a JSON object");
		else if (!ortc::canReceive(*rtpParameters, *this->extendedRtpCapabilities))
			MSC_THROW_UNSUPPORTED_ERROR("cannot consume this Producer");

		// May throw.
		auto result = this->handler->Receive(id, kind, rtpParameters);

		auto& localId = result.first;
		auto* track   = result.second;

		auto* consumer =
		  new Consumer(this, consumerListener, id, localId, producerId, track, *rtpParameters, appData);

		this->consumers[consumer->GetId()] = consumer;

		// If this is the first video Consumer and the Consumer for RTP probation
		// has not yet been created, create it now.
		if (!this->probatorConsumerCreated && kind == "video")
		{
			try
			{
				auto probatorRtpParameters =
				  ortc::generateProbatorRtpParameters(consumer->GetRtpParameters());
				std::string probatorId{ "probator" };

				// May throw.
				auto result = this->handler->Receive(probatorId, kind, &probatorRtpParameters);

				MSC_DEBUG("Consumer for RTP probation created");

				this->probatorConsumerCreated = true;
			}
			catch (std::runtime_error& error)
			{
				MSC_ERROR("failed to create Consumer for RTP probation: %s", error.what());
			}
		}

		return consumer;
	}

	void RecvTransport::Close()
	{
		MSC_TRACE();

		if (this->closed)
			return;

		Transport::Close();

		// Close all Producers.
		for (auto& kv : this->consumers)
		{
			auto* consumer = kv.second;

			consumer->TransportClosed();
		}
	}

	void RecvTransport::OnClose(Consumer* consumer)
	{
		MSC_TRACE();

		this->consumers.erase(consumer->GetId());

		if (this->closed)
			return;

		// May throw.
		this->handler->StopReceiving(consumer->GetLocalId());
	}

	json RecvTransport::OnGetStats(const Consumer* consumer)
	{
		MSC_TRACE();

		if (this->closed)
			MSC_THROW_INVALID_STATE_ERROR("RecvTransport closed");

		return this->handler->GetReceiverStats(consumer->GetLocalId());
	}
} // namespace mediasoupclient
