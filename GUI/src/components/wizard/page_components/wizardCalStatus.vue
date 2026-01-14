<template>
  <div class="cal-message"
    v-tooltip.top="{
            content: tooltip,
            class: 'tooltip-custom tooltip-other-custom fade-in',
            delay: 0,
            visible: !calStatus,
          }"
  >{{message}}</div>
</template>

<script>
export default {
  name: "wizardCalStatus",
  props: {
    data: Object,
    calStatus: Boolean,
  },
  computed: {
    message() {
      if (this.calStatus == true) {
        return this.$t('wizard.calSuccess');
      } else if (this.calStatus == false) {
        return this.$t('wizard.calFailure');
      }
      return "";
    },
    tooltip() {
      // Return translated tooltip based on which calibration failed
      if (this.data.tooltip && this.data.tooltip.includes('motor')) {
        return this.$t('wizard.tooltips.motorCalFail');
      } else if (this.data.tooltip && this.data.tooltip.includes('encoder')) {
        return this.$t('wizard.tooltips.encoderCalFail');
      }
      return this.data.tooltip || "";
    },
  },
};
</script>

<style scoped>
.cal-message {
    margin-top: auto;
    margin-bottom: auto;
}
</style>